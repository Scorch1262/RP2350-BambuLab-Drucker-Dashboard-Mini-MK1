// Bambu Lab - Portierung von PrinterConnection (app.py MK6 v2.5.5)
#include "bambu.h"
#include "mqtt.h"
#include "tls.h"
#include "util.h"
#include "config.h"
#include <map>
#include <stdlib.h>

static const uint32_t PUSHALL_INTERVAL_MS = 300000;   // PUSHALL_INTERVAL_SEC = 300

BambuConn::BambuConn(JsonObjectConst pcfg) : PrinterConn(pcfg) {
    family = pcfg["bambu_family"] | "x1";
    if (!cfg::is_bambu_family(family)) family = "x1";
    access_code = pcfg["access_code"] | "";
    serial = pcfg["serial"] | "";
    mqtt_port = (uint16_t)(pcfg["mqtt_port"] | 8883);
    camera_port = (uint16_t)(pcfg["camera_port"] | 6000);
}

void BambuConn::start() { spawn("bambu", 10 * 1024); }

std::string BambuConn::current_state() {
    plat::Lock lk(m);
    return gcode_state_;
}

bool BambuConn::is_connected() {
    plat::Lock lk(m);
    return connected_;
}

static void put_str_or_null(JsonObject o, const char* k, const std::string& v) {
    if (v.empty()) o[k] = nullptr;
    else o[k] = v;
}

void BambuConn::status_json(JsonObject o) {
    plat::Lock lk(m);
    o["connected"] = connected_;
    put_str_or_null(o, "last_update", last_update_);
    o["gcode_state"] = gcode_state_;
    if (progress_.has) o["progress"] = progress_.v;
    else o["progress"] = 0;
    o["file_name"] = file_name_;
    chamber_.put(o, "chamber_temp");
    nozzle_.put(o, "nozzle_temp");
    bed_.put(o, "bed_temp");
    remaining_.put(o, "remaining_min");
    JsonArray a = o["ams"].to<JsonArray>();
    for (auto& s : ams_) {
        JsonObject t = a.add<JsonObject>();
        t["slot"] = s.slot;
        t["type"] = s.type;
        t["color"] = s.color;
        t["remain"] = s.remain;
    }
    JsonArray u = o["ams_units"].to<JsonArray>();
    for (auto& x : units_) {
        JsonObject t = u.add<JsonObject>();
        t["id"] = x.id;
        if (x.has_h) t["humidity"] = x.humidity;
        else t["humidity"] = nullptr;
        if (x.has_raw) t["humidity_raw"] = x.humidity_raw;
        else t["humidity_raw"] = nullptr;
    }
    if (rtsp_known_) o["ipcam_rtsp_url"] = rtsp_url_;
    else o["ipcam_rtsp_url"] = nullptr;
}

std::string BambuConn::rtsp_url(bool* known) {
    plat::Lock lk(m);
    if (known) *known = rtsp_known_;
    return rtsp_url_;
}

std::vector<AmsSlot> BambuConn::ams_snapshot() {
    plat::Lock lk(m);
    return ams_;
}

// Zahl aus JSON (auch als String geliefert)
static bool num_of(JsonVariantConst v, double& out) {
    if (v.is<double>() || v.is<long>() || v.is<int>()) {
        out = v.as<double>();
        return true;
    }
    if (v.is<const char*>()) {
        const char* s = v.as<const char*>();
        char* end = nullptr;
        double d = strtod(s, &end);
        if (end && end != s) {
            out = d;
            return true;
        }
    }
    return false;
}

static std::string str_of(JsonVariantConst v, const char* def) {
    if (v.is<const char*>()) return v.as<const char*>();
    if (v.is<long>()) return util::fmt("%ld", v.as<long>());
    return def;
}

static std::string argb_to_css(const std::string& hex) {
    if (hex.size() < 6) return "#666666";
    return "#" + hex.substr(0, 6);
}

void BambuConn::apply_report(JsonObjectConst p) {
    plat::Lock lk(m);
    connected_ = true;
    last_update_ = plat::time_hms();
    double d;
    if (p["gcode_state"].is<const char*>()) gcode_state_ = p["gcode_state"].as<const char*>();
    if (num_of(p["mc_percent"], d)) progress_.set(d);
    std::string sub = p["subtask_name"] | "";
    std::string gf = p["gcode_file"] | "";
    if (!sub.empty()) file_name_ = sub;
    else if (!gf.empty()) file_name_ = gf;
    // Kammertemperatur: chamber_temper -> chamber_temp -> device.ctc.info.temp (v2.2.0/v2.2.13)
    if (!p["chamber_temper"].isNull()) {
        if (num_of(p["chamber_temper"], d)) chamber_.set(d);
    } else if (!p["chamber_temp"].isNull()) {
        if (num_of(p["chamber_temp"], d)) chamber_.set(d);
    } else {
        JsonVariantConst info = p["device"]["ctc"]["info"];
        bool found = false;
        if (info.is<JsonObjectConst>()) {
            found = num_of(info["temp"], d);
        } else if (info.is<JsonArrayConst>()) {
            for (JsonVariantConst e : info.as<JsonArrayConst>()) {
                if (e.is<JsonObjectConst>() && num_of(e["temp"], d)) {
                    found = true;
                    break;
                }
            }
        }
        if (found) {
            chamber_.set(d);
            if (!chamber_ctc_logged_) {
                chamber_ctc_logged_ = true;
                logf("[MK6] Kammertemperatur ueber 'device.ctc.info.temp' gefunden: Drucker='%s' Wert=%g",
                     name.c_str(), d);
            }
        } else if (family != "a1" && !chamber_missing_logged_) {
            chamber_missing_logged_ = true;
            logf("[MK6] Hinweis: Drucker '%s' (Familie: %s) liefert kein 'chamber_temper'/'chamber_temp'/"
                 "'device.ctc.info.temp'-Feld im MQTT-Report.", name.c_str(), family.c_str());
        }
    }
    if (num_of(p["nozzle_temper"], d)) nozzle_.set(d);
    if (num_of(p["bed_temper"], d)) bed_.set(d);
    if (num_of(p["mc_remaining_time"], d)) remaining_.set(d);
    JsonVariantConst ipcam = p["ipcam"];
    if (ipcam.is<JsonObjectConst>() && ipcam["rtsp_url"].is<const char*>()) {
        rtsp_known_ = true;
        rtsp_url_ = ipcam["rtsp_url"].as<const char*>();
    }
    JsonVariantConst ams_root = p["ams"]["ams"];
    if (ams_root.is<JsonArrayConst>()) {
        std::vector<AmsSlot> slots;
        std::vector<AmsUnit> units;
        for (JsonVariantConst unit : ams_root.as<JsonArrayConst>()) {
            std::string uid = str_of(unit["id"], "0");
            for (JsonVariantConst tray : unit["tray"].as<JsonArrayConst>()) {
                AmsSlot s;
                s.slot = uid + "-" + str_of(tray["id"], "0");
                std::string tt = tray["tray_type"] | "";
                s.type = tt.empty() ? "-" : tt;
                s.color = argb_to_css(tray["tray_color"] | "");
                double r;
                s.remain = num_of(tray["remain"], r) ? (int)r : -1;
                slots.push_back(s);
            }
            AmsUnit u;
            u.id = uid;
            if (num_of(unit["humidity"], d)) { u.has_h = true; u.humidity = (int)d; }
            if (num_of(unit["humidity_raw"], d)) { u.has_raw = true; u.humidity_raw = (int)d; }
            units.push_back(u);
        }
        ams_ = slots;
        units_ = units;
    }
}

void BambuConn::pause_mqtt() { paused_ = true; }
void BambuConn::resume_mqtt() { paused_ = false; }

bool BambuConn::wait_disconnected(uint32_t ms) {
    uint32_t s = plat::millis();
    while (plat::millis() - s < ms) {
        if (!mqtt_up_) return true;
        plat::sleep_ms(50);
    }
    return !mqtt_up_;
}

bool BambuConn::wait_connected(uint32_t ms) {
    uint32_t s = plat::millis();
    while (plat::millis() - s < ms) {
        if (mqtt_up_ && is_connected()) return true;
        plat::sleep_ms(100);
    }
    return mqtt_up_ && is_connected();
}

bool BambuConn::publish_request(const std::string& payload, uint32_t timeout_ms, std::string* err) {
    if (!mqtt_up_) {
        if (err) *err = "Keine aktive MQTT-Verbindung zum Drucker - Druckauftrag kann nicht gestartet werden.";
        return false;
    }
    auto o = std::make_shared<Out>();
    o->payload = payload;
    {
        plat::Lock lk(outq_m_);
        outq_.push_back(o);
    }
    uint32_t s = plat::millis();
    while (plat::millis() - s < timeout_ms) {
        if (o->state != 0) break;
        plat::sleep_ms(20);
    }
    if (o->state == 1) return true;
    if (err) *err = "MQTT-Befehl zum Druckstart konnte nicht gesendet werden.";
    return false;
}

bool BambuConn::sleep_or_pause(uint32_t ms) {
    uint32_t start = plat::millis();
    while (plat::millis() - start < ms) {
        if (stop_req) return true;
        plat::sleep_ms(100);
    }
    return stop_req;
}

void BambuConn::run() {
    std::string label = name + " / " + ip;
    std::string report_topic = "device/" + serial + "/report";
    std::string request_topic = "device/" + serial + "/request";
    // Filter: nur die ausgewerteten Felder parsen (spart viel RAM bei "pushall")
    JsonDocument filter;
    {
        JsonObject pr = filter["print"].to<JsonObject>();
        for (const char* k : {"gcode_state", "mc_percent", "subtask_name", "gcode_file", "chamber_temper",
                              "chamber_temp", "nozzle_temper", "bed_temper", "mc_remaining_time"})
            pr[k] = true;
        pr["ipcam"]["rtsp_url"] = true;
        pr["device"]["ctc"]["info"] = true;
        JsonObject unit = pr["ams"]["ams"].to<JsonArray>().add<JsonObject>();
        unit["id"] = true;
        unit["humidity"] = true;
        unit["humidity_raw"] = true;
        JsonObject tray = unit["tray"].to<JsonArray>().add<JsonObject>();
        for (const char* k : {"id", "tray_type", "tray_color", "remain"}) tray[k] = true;
    }

    while (!stop_req) {
        if (paused_) {
            // Waehrend eines FTPS-Uploads bewusst pausiert (siehe pause_mqtt())
            plat::sleep_ms(200);
            continue;
        }
        std::string err;
        tls::Options o;
        o.tls12_only = true;
        o.handshake_timeout_ms = 15000;
        tls::TlsConn* c = tls::TlsConn::connect(ip, mqtt_port, 8000, o, &err);
        if (!c) {
            logf("[MK6-MQTT] (%s): Verbindungsfehler: %s", label.c_str(), err.c_str());
            {
                plat::Lock lk(m);
                connected_ = false;
            }
            if (sleep_or_pause(5000)) break;
            continue;
        }
        mqtt::Client cl;
        std::string cid = "dashboard-" + id + "-" + util::new_id(6);
        int rc = cl.connect(c, cid, "bblp", access_code, 30, &err);
        if (rc != 0) {
            if (rc > 0) logf("[MK6-MQTT] (%s): Verbindung ABGELEHNT (rc=%d: %s)", label.c_str(), rc, mqtt::connack_text(rc));
            else logf("[MK6-MQTT] (%s): Verbindungsfehler: %s", label.c_str(), err.c_str());
            {
                plat::Lock lk(m);
                connected_ = false;
            }
            if (sleep_or_pause(5000)) break;
            continue;
        }
        logf("[MK6-MQTT] (%s): Verbindung erfolgreich (rc=0: Connection Accepted)", label.c_str());
        connected_at_ = plat::millis();
        {
            plat::Lock lk(m);
            connected_ = true;
        }
        mqtt_up_ = true;
        cl.subscribe(report_topic);
        logf("[MK6-MQTT] (%s): Topic '%s' abonniert.", label.c_str(), report_topic.c_str());
        if (family != "a1") {
            cl.subscribe(request_topic);
            logf("[MK6-MQTT] (%s): Topic '%s' abonniert.", label.c_str(), request_topic.c_str());
        } else {
            logf("[MK6-MQTT] (%s): Abo des 'request'-Topics uebersprungen (bambu_family=a1, behebt "
                 "sofortigen Verbindungsabbruch - siehe UEBERGABE.md v2.5.4/v2.5.5).", label.c_str());
        }
        cl.publish(request_topic, "{\"pushing\":{\"sequence_id\":\"0\",\"command\":\"pushall\",\"version\":1,\"push_target\":1}}");
        last_pushall_ = plat::millis();
        logf("[MK6-MQTT] (%s): 'pushall'-Anfrage gesendet.", label.c_str());

        bool lost = false;
        while (!stop_req && !paused_) {
            // ausgehende Befehle (Druckstart) senden
            for (;;) {
                std::shared_ptr<Out> out;
                {
                    plat::Lock lk(outq_m_);
                    if (outq_.empty()) break;
                    out = outq_.front();
                    outq_.pop_front();
                }
                out->state = cl.publish(request_topic, out->payload) ? 1 : -1;
            }
            mqtt::Message msg;
            int r = cl.poll(msg, 150);
            if (r < 0) {
                lost = true;
                break;
            }
            if (r == 0) continue;
            if (msg.topic == request_topic) {
                // v2.2.20: fremde/eigene project_file-Kommandos nur protokollieren
                if (msg.payload.find("\"project_file\"") != std::string::npos) {
                    std::string pl = msg.payload.size() > 1500 ? msg.payload.substr(0, 1500) + "..." : msg.payload;
                    logf("[MK6-DIAG] project_file-Kommando auf dem lokalen Broker von '%s' beobachtet: %s",
                         name.c_str(), pl.c_str());
                }
                continue;
            }
            JsonDocument d;
            DeserializationError e = deserializeJson(d, msg.payload, DeserializationOption::Filter(filter));
            if (e) continue;
            if (d["print"].is<JsonObjectConst>()) apply_report(d["print"].as<JsonObjectConst>());
            if (plat::millis() - last_pushall_ > PUSHALL_INTERVAL_MS) {
                cl.publish(request_topic, "{\"pushing\":{\"sequence_id\":\"0\",\"command\":\"pushall\",\"version\":1,\"push_target\":1}}");
                last_pushall_ = plat::millis();
            }
        }
        mqtt_up_ = false;
        if (lost) {
            float standzeit = (plat::millis() - connected_at_) / 1000.0f;
            logf("[MK6-MQTT] (%s): Verbindung unerwartet getrennt (%s), Standzeit seit Connect: %.1fs.",
                 label.c_str(), cl.last_error.c_str(), standzeit);
        }
        cl.disconnect();
        {
            plat::Lock lk(m);
            connected_ = false;
        }
        // offene Sendeauftraege als fehlgeschlagen markieren
        {
            plat::Lock lk(outq_m_);
            for (auto& o2 : outq_) o2->state = -1;
            outq_.clear();
        }
        if (stop_req) break;
        if (!paused_ && sleep_or_pause(3000)) break;
    }
    mqtt_up_ = false;
    logf("[MK6-MQTT] (%s): Verbindungsaufgabe beendet.", label.c_str());
}

// =====================================================================
// FTPS-Upload + Druckstart
// =====================================================================
namespace bambu {

Profile profile_by_name(const std::string& n) {
    if (n == "a1") return {"a1", false, false, true};
    return {"x1", true, true, false};
}

std::string family_to_profile(const std::string& family) {
    // BAMBU_FAMILY_TO_FTPS_PROFILE: nur a1 -> a1, alle anderen vorerst x1
    return family == "a1" ? "a1" : "x1";
}

struct Slot {
    std::shared_ptr<BambuConn> p;
    Profile prof;
    tls::TlsConn* ctrl = nullptr;
    tls::TlsConn* data = nullptr;
    std::string remote;
    size_t size = 0;
    std::vector<int> mapping;
    bool has_mapping = false;
    uint32_t created = 0;
    bool busy = false;
};

static plat::Mutex* g_m = nullptr;
static std::map<std::string, std::shared_ptr<Slot>>* g_slots = nullptr;

static void ensure() {
    if (!g_m) g_m = new plat::Mutex();
    if (!g_slots) g_slots = new std::map<std::string, std::shared_ptr<Slot>>();
}

// FTP-Antwort lesen (inkl. mehrzeiliger Antworten "123-...")
static int ftp_reply(plat::Conn* c, std::string* text, uint32_t timeout_ms) {
    std::string line;
    if (c->read_line(line, 1024, timeout_ms) != 1) {
        if (text) *text = "keine Antwort vom Drucker (Zeitueberschreitung oder Verbindung geschlossen)";
        return -1;
    }
    if (line.size() < 3) return -1;
    int code = atoi(line.substr(0, 3).c_str());
    std::string all = line;
    if (line.size() > 3 && line[3] == '-') {
        std::string end = line.substr(0, 3) + " ";
        for (int i = 0; i < 50; i++) {
            if (c->read_line(line, 1024, timeout_ms) != 1) break;
            all += " | " + line;
            if (util::starts_with(line, end)) break;
        }
    }
    if (text) *text = all;
    return code;
}

static int ftp_cmd(plat::Conn* c, const std::string& cmd, std::string* text, uint32_t timeout_ms = 15000) {
    if (!c->write_str(cmd + "\r\n")) {
        if (text) *text = "Senden von '" + cmd.substr(0, 4) + "' fehlgeschlagen";
        return -1;
    }
    return ftp_reply(c, text, timeout_ms);
}

static void slot_cleanup(Slot& s, bool resume) {
    if (s.data) {
        s.data->set_close_notify(false);
        delete s.data;
        s.data = nullptr;
    }
    if (s.ctrl) {
        s.ctrl->set_close_notify(false);
        delete s.ctrl;
        s.ctrl = nullptr;
    }
    if (resume && s.p) s.p->resume_mqtt();
}

static const char* MSG_553 =
    "Der Drucker hat das Anlegen der Datei abgelehnt (FTP 553 \"Could not create file\") - die Verbindung "
    "selbst funktioniert, aber am Drucker ist kein beschreibbarer Speicher verfuegbar.\n"
    "Bei H2D/H2S (und P2S): Uploads per LAN/FTPS landen AUSSCHLIESSLICH auf einem eingesteckten USB-Stick, "
    "nicht im internen Speicher - bitte einen USB-Stick (FAT32 oder exFAT, mind. USB 2.0) einstecken und am "
    "Druckerdisplay pruefen, dass er erkannt wird.\n"
    "Bei X1/P1/A1: pruefen, ob eine microSD-Karte eingesteckt, erkannt und nicht voll/schreibgeschuetzt ist.";

std::string upload_begin(std::shared_ptr<BambuConn> p, const std::string& remote_name, size_t size,
                         const std::string& profile, const std::vector<int>& mapping, bool has_mapping,
                         std::string* err, std::string* err_code) {
    ensure();
    housekeeping();
    {
        plat::Lock lk(*g_m);
        for (auto& kv : *g_slots) {
            if (kv.second->p == p) {
                if (err) *err = "Fuer diesen Drucker laeuft bereits ein Upload.";
                return "";
            }
        }
    }
    auto s = std::make_shared<Slot>();
    s->p = p;
    s->prof = profile_by_name(profile.empty() ? family_to_profile(p->family) : profile);
    s->remote = remote_name;
    s->size = size;
    s->mapping = mapping;
    s->has_mapping = has_mapping;
    s->created = plat::millis();

    // MQTT waehrend des Uploads trennen (v1.5.2, siehe pause_mqtt() im Original)
    p->pause_mqtt();
    p->wait_disconnected(6000);
    if (!plat::heap_ok(64 * 1024)) {
        if (err) *err = "Zu wenig freier Speicher auf dem Board fuer den Upload - bitte offene Kamera-Fenster schliessen und erneut versuchen.";
        p->resume_mqtt();
        return "";
    }

    std::string step, text;
    tls::Options o;
    o.tls12_only = s->prof.cap_tls12;
    o.allow_tls13 = !s->prof.cap_tls12;
    o.handshake_timeout_ms = 20000;
    std::string e;
    logf("[MK6] FTPS-Upload (Drucker='%s', Profil=%s): verbinde mit %s:990 ...", p->name.c_str(),
         s->prof.name.c_str(), p->ip.c_str());
    s->ctrl = tls::TlsConn::connect(p->ip, 990, 10000, o, &e);
    if (!s->ctrl) {
        if (err) *err = "Steuerverbindung (Port 990): " + e;
        slot_cleanup(*s, true);
        return "";
    }
    logf("[MK6] FTPS: Steuerverbindung steht (%s, %s)", s->ctrl->version().c_str(), s->ctrl->cipher().c_str());
    int code = ftp_reply(s->ctrl, &text, 15000);
    if (code != 220) { step = "Begruessung"; goto fail; }
    code = ftp_cmd(s->ctrl, "USER bblp", &text);
    if (code != 331 && code != 230) { step = "USER"; goto fail; }
    if (code == 331) {
        code = ftp_cmd(s->ctrl, "PASS " + p->access_code, &text);
        if (code != 230) { step = "Anmeldung (Access Code pruefen)"; goto fail; }
    }
    code = ftp_cmd(s->ctrl, "PBSZ 0", &text);
    if (code != 200) { step = "PBSZ"; goto fail; }
    code = ftp_cmd(s->ctrl, "PROT P", &text);
    if (code != 200) { step = "PROT P"; goto fail; }
    code = ftp_cmd(s->ctrl, "TYPE I", &text);
    if (code != 200) { step = "TYPE I"; goto fail; }
    {
        code = ftp_cmd(s->ctrl, "PASV", &text);
        if (code != 227) { step = "PASV"; goto fail; }
        // "227 Entering Passive Mode (h1,h2,h3,h4,p1,p2)"
        size_t lp = text.find('('), rp = text.find(')');
        std::vector<std::string> nums;
        if (lp != std::string::npos && rp != std::string::npos && rp > lp)
            nums = util::split(text.substr(lp + 1, rp - lp - 1), ',');
        if (nums.size() != 6) { step = "PASV-Antwort"; goto fail; }
        uint16_t dport = (uint16_t)(atoi(nums[4].c_str()) * 256 + atoi(nums[5].c_str()));
        // Wie ftplib: zur IP des Druckers verbinden (nicht zur gemeldeten IP)
        plat::Conn* tcp = plat::tcp_connect(p->ip, dport, 10000, &e);
        if (!tcp) {
            text = e;
            step = util::fmt("Datenverbindung (PASV-Port %u)", dport);
            goto fail;
        }
        code = ftp_cmd(s->ctrl, "STOR " + remote_name, &text, 20000);
        if (code != 150 && code != 125) {
            delete tcp;
            if (code == 553) {
                if (err_code) *err_code = "553";
                if (err) *err = std::string(MSG_553) + "\nDetails: Profil " + s->prof.name + ": " + text;
                slot_cleanup(*s, true);
                return "";
            }
            step = "STOR";
            goto fail;
        }
        tls::Session sess;
        if (s->prof.reuse_session) s->ctrl->save_session(sess);
        s->data = tls::TlsConn::wrap(tcp, o, &e, s->prof.reuse_session ? &sess : nullptr);
        if (!s->data) {
            text = e;
            step = "TLS-Datenverbindung";
            goto fail;
        }
        logf("[MK6] FTPS: Datenverbindung steht (%s, Sitzung %s), Upload von '%s' (%u Bytes) kann beginnen.",
             s->data->version().c_str(), s->prof.reuse_session ? "wiederverwendet" : "neu", remote_name.c_str(),
             (unsigned)size);
    }
    {
        std::string sid = util::new_id(16);
        plat::Lock lk(*g_m);
        (*g_slots)[sid] = s;
        return sid;
    }
fail:
    if (err) *err = util::fmt("Profil %s, Schritt '%s': %s", s->prof.name.c_str(), step.c_str(), text.c_str());
    logf("[MK6] FTPS-Upload fehlgeschlagen (Drucker='%s'): %s", p->name.c_str(), err ? err->c_str() : "");
    slot_cleanup(*s, true);
    return "";
}

static std::shared_ptr<Slot> take_slot(const std::string& sid) {
    ensure();
    plat::Lock lk(*g_m);
    auto it = g_slots->find(sid);
    if (it == g_slots->end() || it->second->busy) return nullptr;
    it->second->busy = true;
    return it->second;
}

static void drop_slot(const std::string& sid) {
    plat::Lock lk(*g_m);
    g_slots->erase(sid);
}

static std::string job_name_of(const std::string& remote) {
    // os.path.splitext() zweimal: "teil.gcode.3mf" -> "teil"
    std::string n = remote;
    for (int i = 0; i < 2; i++) {
        size_t dot = n.rfind('.');
        if (dot != std::string::npos && dot > 0) n = n.substr(0, dot);
    }
    return n.empty() ? remote : n;
}

bool upload_finish(const std::string& sid, plat::Conn* src, size_t size, std::string* err, std::string* result_json) {
    std::shared_ptr<Slot> s = take_slot(sid);
    if (!s) {
        if (err) *err = "Upload-Vorbereitung nicht gefunden oder abgelaufen - bitte erneut versuchen.";
        return false;
    }
    std::shared_ptr<BambuConn> p = s->p;
    size_t total = s->size ? s->size : size;
    size_t sent = 0;
    uint8_t* buf = (uint8_t*)malloc(4096);
    bool ok = buf != nullptr;
    std::string why;
    uint32_t last_log = plat::millis();
    while (ok && sent < total) {
        size_t want = total - sent < 4096 ? total - sent : 4096;
        int n = src->read(buf, want, 30000);
        if (n <= 0) {
            ok = false;
            why = "Datei-Upload vom Browser abgebrochen";
            break;
        }
        if (!s->data->write_all(buf, (size_t)n, 60000)) {
            ok = false;
            why = "Datenverbindung zum Drucker abgebrochen";
            break;
        }
        sent += (size_t)n;
        if (plat::millis() - last_log > 10000) {
            last_log = plat::millis();
            logf("[MK6] FTPS: %u / %u Bytes uebertragen", (unsigned)sent, (unsigned)total);
        }
    }
    free(buf);
    if (!ok) {
        unsigned pct = total ? (unsigned)(sent * 100 / total) : 0;
        if (err)
            *err = util::fmt("Verbindung ist waehrend der Dateiuebertragung abgebrochen (bei %u/%u Bytes, %u%%): %s. "
                             "Die Datei ist damit unvollstaendig auf dem Drucker gelandet (falls ueberhaupt).",
                             (unsigned)sent, (unsigned)total, pct, why.c_str());
        logf("[MK6] FTPS-Upload abgebrochen (Drucker='%s'): %s", p->name.c_str(), why.c_str());
        slot_cleanup(*s, true);
        drop_slot(sid);
        return false;
    }
    // Profil "a1": Datenverbindung OHNE TLS-Abschluss schliessen (skip_unwrap, v1.6.0)
    s->data->set_close_notify(!s->prof.skip_unwrap);
    delete s->data;
    s->data = nullptr;
    std::string text;
    int code = ftp_reply(s->ctrl, &text, 90000);
    if (code < 200 || code >= 300) {
        if (err) *err = util::fmt("Drucker hat die Uebertragung nicht bestaetigt (Profil %s): %s",
                                  s->prof.name.c_str(), text.c_str());
        logf("[MK6] FTPS: keine 226-Bestaetigung (Drucker='%s'): %s", p->name.c_str(), text.c_str());
        slot_cleanup(*s, true);
        drop_slot(sid);
        return false;
    }
    logf("[MK6] FTPS: Upload bestaetigt (%s)", text.c_str());
    ftp_cmd(s->ctrl, "QUIT", &text, 3000);
    slot_cleanup(*s, false);
    drop_slot(sid);

    // MQTT wieder verbinden und Druck starten (_request_print())
    p->resume_mqtt();
    if (!p->wait_connected(15000)) {
        if (err) *err = "Keine aktive MQTT-Verbindung zum Drucker - Druckauftrag kann nicht gestartet werden.";
        return false;
    }
    bool use_ams = false;
    int matched = 0;
    for (int m : s->mapping)
        if (m >= 0) { use_ams = true; matched++; }
    if (!s->has_mapping) use_ams = false;

    JsonDocument d;
    JsonObject pr = d["print"].to<JsonObject>();
    std::string job = job_name_of(s->remote);
    std::string url = (p->family == "h2" || p->family == "p2") ? "ftp:///" + s->remote : "file:///sdcard/" + s->remote;
    pr["sequence_id"] = "0";
    pr["command"] = "project_file";
    pr["param"] = "Metadata/plate_1.gcode";
    pr["url"] = url;
    pr["bed_type"] = "auto";
    pr["project_id"] = "0";
    pr["profile_id"] = "0";
    pr["task_id"] = "0";
    pr["subtask_id"] = "0";
    pr["subtask_name"] = job;
    pr["use_ams"] = use_ams;
    pr["timelapse"] = false;
    pr["flow_cali"] = true;
    pr["bed_leveling"] = true;
    pr["layer_inspect"] = true;
    pr["vibration_cali"] = true;
    if (s->has_mapping) {
        JsonArray am = pr["ams_mapping"].to<JsonArray>();
        for (int m : s->mapping) am.add(m);
        if (p->family == "h2") {
            JsonArray am2 = pr["ams_mapping2"].to<JsonArray>();
            for (int m : s->mapping) {
                JsonObject e = am2.add<JsonObject>();
                if (m < 0) { e["ams_id"] = 255; e["slot_id"] = 255; }
                else if (m >= 128) { e["ams_id"] = m; e["slot_id"] = 0; }
                else { e["ams_id"] = m / 4; e["slot_id"] = m % 4; }
            }
        }
    }
    std::string payload;
    serializeJson(d, payload);
    logf("[MK6] Druckstart angefordert: Drucker='%s' (Familie: %s) url='%s' param='Metadata/plate_1.gcode' subtask_name='%s'",
         p->name.c_str(), p->family.c_str(), url.c_str(), job.c_str());
    if (!p->publish_request(payload, 10000, err)) return false;

    JsonDocument r;
    r["use_ams"] = use_ams;
    if (s->has_mapping) {
        JsonArray a = r["mapping"].to<JsonArray>();
        for (int m : s->mapping) a.add(m);
    } else {
        r["mapping"] = nullptr;
    }
    r["total"] = s->has_mapping ? (int)s->mapping.size() : 0;
    r["matched"] = matched;
    if (result_json) serializeJson(r, *result_json);
    return true;
}

void upload_cancel(const std::string& sid) {
    ensure();
    std::shared_ptr<Slot> s;
    {
        plat::Lock lk(*g_m);
        auto it = g_slots->find(sid);
        if (it == g_slots->end() || it->second->busy) return;
        s = it->second;
        g_slots->erase(it);
    }
    slot_cleanup(*s, true);
}

void housekeeping() {
    ensure();
    std::vector<std::shared_ptr<Slot>> stale;
    {
        plat::Lock lk(*g_m);
        for (auto it = g_slots->begin(); it != g_slots->end();) {
            if (!it->second->busy && plat::millis() - it->second->created > 60000) {
                stale.push_back(it->second);
                it = g_slots->erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& s : stale) {
        logf("[MK6] FTPS: vorbereiteter Upload fuer '%s' verfallen (kein Dateiempfang innerhalb 60s).", s->p->name.c_str());
        slot_cleanup(*s, true);
    }
}

} // namespace bambu
