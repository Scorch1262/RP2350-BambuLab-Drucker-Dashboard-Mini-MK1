// Formlabs / OctoPrint / Creality / Ultimaker - Portierung der jeweiligen
// *Connection-Klassen aus app.py (MK6 v2.5.5)
#include "pollers.h"
#include "config.h"
#include "tls.h"
#include "util.h"
#include <map>
#include <ctype.h>
#include <math.h>
#include <stdlib.h>

static double round1(double v) { return round(v * 10.0) / 10.0; }

static bool num_of(JsonVariantConst v, double& out) {
    if (v.is<double>() || v.is<long>()) {
        out = v.as<double>();
        return true;
    }
    if (v.is<const char*>()) {
        const char* s = v.as<const char*>();
        char* end = nullptr;
        double d = strtod(s, &end);
        if (end && end != s && *end == 0) {
            out = d;
            return true;
        }
    }
    return false;
}

static std::string json_to_text(JsonVariantConst v) {
    if (v.is<const char*>()) return v.as<const char*>();
    std::string s;
    serializeJson(v, s);
    return s;
}

// ---------------------------------------------------------------------
void PollConn::start() { spawn("poll", 8 * 1024); }

std::string PollConn::current_state() {
    plat::Lock lk(m);
    return gcode_state_;
}

void PollConn::set_error(const std::string& e) {
    plat::Lock lk(m);
    connected_ = false;
    has_err_ = true;
    err_ = e;
}

void PollConn::run() {
    before_loop();
    while (!stop_req) {
        poll_once();
        if (sleep_stoppable(interval_ms())) break;
    }
}

void PollConn::common_json(JsonObject o, bool with_temps) {
    o["connected"] = connected_;
    if (last_update_.empty()) o["last_update"] = nullptr;
    else o["last_update"] = last_update_;
    o["gcode_state"] = gcode_state_;
    if (progress_.has) o["progress"] = progress_.v;
    else o["progress"] = 0;
    o["file_name"] = file_name_;
    if (with_temps) {
        chamber_.put(o, "chamber_temp");
        nozzle_.put(o, "nozzle_temp");
        bed_.put(o, "bed_temp");
        remaining_.put(o, "remaining_min");
    }
    if (has_err_) o["error"] = err_;
    else o["error"] = nullptr;
}

// =====================================================================
// Formlabs (PreFormServer / "Formlabs Local API")
// =====================================================================
static const char* FL_PROGRESS_KEYS[] = {"progress_percentage", "percent_complete", "progress", "print_progress",
                                         "completion_percentage", "percentage", nullptr};
static const char* FL_FILE_KEYS[] = {"job_name", "print_file_name", "current_job_name", "file_name", "job",
                                     "print_name", "current_job", nullptr};
static const char* FL_MATERIAL_KEYS[] = {"material_name", "material", "cartridge_material", "resin_type",
                                         "material_code", "tank_material", nullptr};
static const char* FL_STATE_KEYS[] = {"status", "state", "print_status", "device_status", "machine_state", nullptr};

static bool key_in(const char* k, const char** keys) {
    std::string lk = util::lower(k);
    for (int i = 0; keys[i]; i++)
        if (lk == keys[i]) return true;
    return false;
}

// _find_first(): erstes (Tiefensuche) Schluessel/Wert-Paar mit passendem
// Schluessel und Wert != None/""
static bool find_first(JsonVariantConst v, const char** keys, JsonVariantConst& out) {
    if (v.is<JsonObjectConst>()) {
        for (JsonPairConst kv : v.as<JsonObjectConst>()) {
            JsonVariantConst val = kv.value();
            if (key_in(kv.key().c_str(), keys) && !val.isNull() &&
                !(val.is<const char*>() && val.as<const char*>()[0] == 0)) {
                out = val;
                return true;
            }
            if (find_first(val, keys, out)) return true;
        }
    } else if (v.is<JsonArrayConst>()) {
        for (JsonVariantConst e : v.as<JsonArrayConst>())
            if (find_first(e, keys, out)) return true;
    }
    return false;
}

static std::string preform_url() {
    plat::Lock lk(cfg::mtx());
    return cfg::doc()["preform_server"] | "";
}

bool FormlabsConn::get_json(const std::string& method, const std::string& path, const std::string& body,
                            JsonDocument& out, std::string* err) {
    std::string base = preform_url();
    util::Url u = util::parse_url(base);
    if (!u.ok) {
        if (err) *err = "PreFormServer-Adresse ist nicht eingestellt (Einstellungen -> Allgemein, z. B. http://192.168.1.10:44388).";
        return false;
    }
    std::string bp = u.path;
    while (!bp.empty() && bp.back() == '/') bp.pop_back();
    httpc::Response r;
    httpc::Headers h = {{"Content-Type", "application/json"}};
    if (!httpc::request(u.host, u.port, u.scheme == "https", method, bp + path, h, body, r, method == "POST" ? 15000 : 6000)) {
        if (err) *err = "PreFormServer unter " + base + " nicht erreichbar. Laeuft PreFormServer.exe im Hintergrund?";
        return false;
    }
    if (r.status >= 400) {
        if (err) *err = "PreFormServer unter " + base + " nicht erreichbar. Laeuft PreFormServer.exe im Hintergrund?";
        return false;
    }
    out.clear();
    if (r.body.empty()) return true;
    DeserializationError e = deserializeJson(out, r.body);
    if (e) {
        if (err) *err = std::string("Antwort des PreFormServer nicht lesbar: ") + e.c_str();
        return false;
    }
    return true;
}

void FormlabsConn::before_loop() {
    JsonDocument d;
    std::string body = "{\"ip_address\":\"" + util::json_escape(ip) + "\",\"timeout_seconds\":8}";
    get_json("POST", "/discover-devices/", body, d, nullptr);
}

void FormlabsConn::poll_once() {
    JsonDocument data;
    std::string err;
    if (!get_json("GET", "/devices/", "", data, &err)) {
        set_error(err);
        return;
    }
    JsonObjectConst match;
    for (JsonObjectConst dv : data["devices"].as<JsonArrayConst>()) {
        if (ip == (dv["ip_address"] | "")) {
            match = dv;
            break;
        }
    }
    if (match.isNull()) {
        set_error("Geraet mit dieser IP wurde vom PreFormServer (noch) nicht gefunden. Pruefen: PreFormServer laeuft, "
                  "Geraet ist eingeschaltet und im gleichen Netzwerk erreichbar.");
        return;
    }
    device_id_ = json_to_text(match["id"]);
    JsonDocument detail_doc;
    JsonVariantConst detail = match;
    if (!device_id_.empty() && !match["id"].isNull()) {
        if (get_json("GET", "/devices/" + device_id_ + "/", "", detail_doc, nullptr)) detail = detail_doc.as<JsonVariantConst>();
    }
    plat::Lock lk(m);
    has_err_ = false;
    err_.clear();
    connected_ = match["is_connected"].isNull() ? true : match["is_connected"].as<bool>();
    JsonVariantConst v;
    if (find_first(detail, FL_STATE_KEYS, v)) device_status_ = util::upper(json_to_text(v));
    double d;
    if (find_first(detail, FL_PROGRESS_KEYS, v) && num_of(v, d)) {
        if (d <= 1) d *= 100;
        progress_.set(round1(d));
    }
    if (find_first(detail, FL_FILE_KEYS, v)) file_name_ = json_to_text(v);
    if (type == "formlabs" && find_first(detail, FL_MATERIAL_KEYS, v)) material_ = json_to_text(v);
    last_update_ = plat::time_hms();
}

std::string FormlabsConn::current_state() {
    plat::Lock lk(m);
    return device_status_;
}

void FormlabsConn::status_json(JsonObject o) {
    plat::Lock lk(m);
    o["connected"] = connected_;
    if (last_update_.empty()) o["last_update"] = nullptr;
    else o["last_update"] = last_update_;
    if (progress_.has) o["progress"] = progress_.v;
    else o["progress"] = 0;
    o["file_name"] = file_name_;
    o["material"] = material_;
    o["device_status"] = device_status_;
    if (has_err_) o["error"] = err_;
    else o["error"] = nullptr;
}

// =====================================================================
// OctoPrint
// =====================================================================
void OctoPrintConn::poll_once() {
    bool https = cfg_["https"] | false;
    uint16_t port = (uint16_t)(cfg_["port"] | 80);
    std::string key = cfg_["api_key"] | "";
    httpc::Headers h = {{"X-Api-Key", key}, {"Accept", "application/json"}};
    httpc::Response r1, r2;
    if (!httpc::request(ip, port, https, "GET", "/api/printer", h, "", r1) ||
        !httpc::request(ip, port, https, "GET", "/api/job", h, "", r2)) {
        set_error("OctoPrint nicht erreichbar (IP/Port pruefen).");
        return;
    }
    int bad = r1.status >= 400 ? r1.status : (r2.status >= 400 ? r2.status : 0);
    if (bad == 403) {
        set_error("OctoPrint hat den API-Key abgelehnt (403).");
        return;
    }
    if (bad) {
        set_error(util::fmt("OctoPrint HTTP-Fehler %d.", bad));
        return;
    }
    JsonDocument printer, job;
    if (deserializeJson(printer, r1.body) || deserializeJson(job, r2.body)) {
        set_error("Antwort von OctoPrint nicht lesbar.");
        return;
    }
    plat::Lock lk(m);
    connected_ = true;
    has_err_ = false;
    err_.clear();
    gcode_state_ = util::upper(printer["state"]["text"] | "UNKNOWN");
    double d;
    if (num_of(printer["temperature"]["tool0"]["actual"], d)) nozzle_.set(d);
    if (num_of(printer["temperature"]["bed"]["actual"], d)) bed_.set(d);
    std::string fn = job["job"]["file"]["name"] | "";
    if (!fn.empty()) file_name_ = fn;
    if (num_of(job["progress"]["completion"], d)) progress_.set(round1(d));
    if (num_of(job["progress"]["printTimeLeft"], d)) remaining_.set(round(d / 60.0));
    last_update_ = plat::time_hms();
}

void OctoPrintConn::status_json(JsonObject o) {
    plat::Lock lk(m);
    common_json(o, true);
    o["ams"].to<JsonArray>();
}

// =====================================================================
// Creality / Klipper (Moonraker)
// =====================================================================
void CrealityConn::before_loop() {
    uint16_t port = (uint16_t)(cfg_["port"] | 7125);
    std::string key = cfg_["api_key"] | "";
    httpc::Headers h;
    if (!key.empty()) h.push_back({"X-Api-Key", key});
    httpc::Response r;
    chamber_obj_.clear();
    if (!httpc::request(ip, port, false, "GET", "/printer/objects/list", h, "", r) || r.status != 200) return;
    JsonDocument d;
    if (deserializeJson(d, r.body)) return;
    for (JsonVariantConst o : d["result"]["objects"].as<JsonArrayConst>()) {
        std::string n = o | "";
        std::string l = util::lower(n);
        if (util::starts_with(l, "temperature_sensor") && l.find("chamber") != std::string::npos) {
            chamber_obj_ = n;
            break;
        }
    }
}

void CrealityConn::poll_once() {
    uint16_t port = (uint16_t)(cfg_["port"] | 7125);
    std::string key = cfg_["api_key"] | "";
    httpc::Headers h;
    if (!key.empty()) h.push_back({"X-Api-Key", key});
    std::string q = "print_stats&display_status&extruder&heater_bed";
    if (!chamber_obj_.empty()) q += "&" + util::url_encode(chamber_obj_);
    httpc::Response r;
    if (!httpc::request(ip, port, false, "GET", "/printer/objects/query?" + q, h, "", r)) {
        set_error("Moonraker nicht erreichbar (IP/Port pruefen, laeuft Moonraker auf dem Drucker?).");
        return;
    }
    if (r.status >= 400) {
        set_error(util::fmt("Moonraker HTTP-Fehler %d.", r.status));
        return;
    }
    JsonDocument d;
    if (deserializeJson(d, r.body)) {
        set_error("Antwort von Moonraker nicht lesbar.");
        return;
    }
    JsonVariantConst st = d["result"]["status"];
    plat::Lock lk(m);
    connected_ = true;
    has_err_ = false;
    err_.clear();
    std::string state = st["print_stats"]["state"] | "";
    if (!state.empty()) gcode_state_ = util::upper(state);
    std::string fn = st["print_stats"]["filename"] | "";
    if (!fn.empty()) file_name_ = fn;
    double v;
    if (num_of(st["display_status"]["progress"], v)) progress_.set(round1(v * 100));
    if (num_of(st["extruder"]["temperature"], v)) nozzle_.set(round1(v));
    if (num_of(st["heater_bed"]["temperature"], v)) bed_.set(round1(v));
    if (!chamber_obj_.empty() && num_of(st[chamber_obj_.c_str()]["temperature"], v)) chamber_.set(round1(v));
    last_update_ = plat::time_hms();
}

void CrealityConn::status_json(JsonObject o) {
    plat::Lock lk(m);
    common_json(o, true);
    o["ams"].to<JsonArray>();
}

// =====================================================================
// Ultimaker (lokale REST-API /api/v1)
// =====================================================================
uint16_t UltimakerConn::port() { return (uint16_t)(cfg_["port"] | 80); }

void UltimakerConn::poll_once() {
    httpc::Headers h = {{"Accept", "application/json"}};
    httpc::Response r;
    if (!httpc::request(ip, port(), false, "GET", "/api/v1/printer/status", h, "", r) || r.status >= 400) {
        set_error("Drucker nicht erreichbar (IP/Port pruefen).");
        return;
    }
    JsonDocument st;
    deserializeJson(st, r.body);
    std::string state = st.is<const char*>() ? st.as<const char*>() : "";
    httpc::Response rj;
    bool job_ok = httpc::request(ip, port(), false, "GET", "/api/v1/print_job", h, "", rj);
    JsonDocument job;
    bool has_job = false;
    if (job_ok && rj.status == 200 && !deserializeJson(job, rj.body) && job.is<JsonObject>()) has_job = true;
    else if (job_ok && rj.status != 404 && rj.status >= 400) {
        set_error(util::fmt("HTTP Error %d", rj.status));
        return;
    } else if (!job_ok) {
        set_error("Drucker nicht erreichbar (IP/Port pruefen).");
        return;
    }
    httpc::Response rb, rh;
    JsonDocument bed, hot;
    bool bed_ok = httpc::request(ip, port(), false, "GET", "/api/v1/printer/bed/temperature", h, "", rb) &&
                  rb.status == 200 && !deserializeJson(bed, rb.body);
    bool hot_ok = httpc::request(ip, port(), false, "GET", "/api/v1/printer/heads/0/extruders/0/hotend/temperature",
                                 h, "", rh) && rh.status == 200 && !deserializeJson(hot, rh.body);
    plat::Lock lk(m);
    connected_ = true;
    has_err_ = false;
    err_.clear();
    if (!state.empty()) gcode_state_ = util::upper(state);
    double d;
    if (has_job) {
        std::string nm = job["name"] | "";
        if (!nm.empty()) file_name_ = nm;
        if (num_of(job["progress"], d)) progress_.set(round1(d * 100));
        double el, tot;
        if (num_of(job["time_elapsed"], el) && num_of(job["time_total"], tot) && tot > el)
            remaining_.set(round((tot - el) / 60.0));
    } else {
        file_name_ = "-";
        progress_.set(0);
        remaining_.clear();
    }
    if (bed_ok && num_of(bed["current"], d)) bed_.set(round1(d));
    if (hot_ok && num_of(hot["current"], d)) nozzle_.set(round1(d));
    last_update_ = plat::time_hms();
}

void UltimakerConn::status_json(JsonObject o) {
    plat::Lock lk(m);
    common_json(o, true);
}

// ---------------------------------------------------------------------
// Ultimaker: Kopplung + Druckauftrag (Digest-Auth)
// ---------------------------------------------------------------------
namespace ultimaker {

struct Pending {
    std::string id, key;
    uint32_t started;
};
static plat::Mutex* g_m = nullptr;
static std::map<std::string, Pending>* g_pending = nullptr;

static void ensure();

static bool printer_addr(const std::string& pid, std::string* ip, uint16_t* port, std::string* err) {
    plat::Lock lk(cfg::mtx());
    JsonObject p = cfg::find_printer(pid);
    if (p.isNull() || std::string(p["type"] | "") != "ultimaker") {
        if (err) *err = "Kein Ultimaker-Drucker mit dieser ID gefunden.";
        return false;
    }
    *ip = p["ip"] | "";
    *port = (uint16_t)(p["port"] | 80);
    return true;
}

bool pair_start(const std::string& pid, std::string* err) {
    ensure();
    std::string ip;
    uint16_t port;
    if (!printer_addr(pid, &ip, &port, err)) return false;
    httpc::Response r;
    httpc::Headers h = {{"Content-Type", "application/x-www-form-urlencoded"}, {"Accept", "application/json"}};
    if (!httpc::request(ip, port, false, "POST", "/api/v1/auth/request", h, "application=DruckerDashboard&user=dashboard",
                        r, 10000)) {
        if (err) *err = "Kopplungsanfrage fehlgeschlagen: " + r.error;
        return false;
    }
    JsonDocument d;
    if (r.status >= 400 || deserializeJson(d, r.body) || !d["id"].is<const char*>() || !d["key"].is<const char*>()) {
        if (err) *err = util::fmt("Kopplungsanfrage fehlgeschlagen: HTTP %d %s", r.status, r.body.substr(0, 200).c_str());
        return false;
    }
    plat::Lock lk(*g_m);
    (*g_pending)[pid] = {d["id"].as<const char*>(), d["key"].as<const char*>(), plat::millis()};
    return true;
}

bool pair_status(const std::string& pid, std::string* status, std::string* err) {
    ensure();
    Pending pd;
    {
        plat::Lock lk(*g_m);
        auto it = g_pending->find(pid);
        if (it == g_pending->end()) {
            if (err) *err = "Keine laufende Kopplungsanfrage fuer diesen Drucker.";
            return false;
        }
        pd = it->second;
        if (plat::millis() - pd.started > 120000) {
            g_pending->erase(it);
            if (err) *err = "Kopplungsanfrage abgelaufen (keine Bestaetigung am Display innerhalb von 2 Minuten) - bitte erneut versuchen.";
            return false;
        }
    }
    std::string ip;
    uint16_t port;
    if (!printer_addr(pid, &ip, &port, err)) return false;
    httpc::Response r;
    if (!httpc::request(ip, port, false, "GET", "/api/v1/auth/check/" + pd.id, {{"Accept", "application/json"}}, "", r)) {
        if (err) *err = "Fehler beim Pruefen der Kopplung: " + r.error;
        return false;
    }
    JsonDocument d;
    deserializeJson(d, r.body);
    std::string msg = d["message"] | "unknown";
    if (msg == "authorized") {
        {
            plat::Lock lk(*g_m);
            g_pending->erase(pid);
        }
        {
            plat::Lock lk(cfg::mtx());
            JsonObject p = cfg::find_printer(pid);
            if (!p.isNull()) {
                p["ultimaker_auth_id"] = pd.id;
                p["ultimaker_auth_key"] = pd.key;
            }
        }
        cfg::mark_dirty();
        *status = "authorized";
        return true;
    }
    if (msg == "unauthorized") {
        plat::Lock lk(*g_m);
        g_pending->erase(pid);
        *status = "unauthorized";
        return true;
    }
    *status = "pending";
    return true;
}

// WWW-Authenticate: Digest realm="...", nonce="...", qop="auth"
static std::map<std::string, std::string> parse_digest(const std::string& hv) {
    std::map<std::string, std::string> m;
    size_t sp = hv.find(' ');
    std::string body = sp == std::string::npos ? hv : hv.substr(sp + 1);
    size_t i = 0;
    while (i < body.size()) {
        while (i < body.size() && (body[i] == ' ' || body[i] == ',')) i++;
        size_t k0 = i;
        while (i < body.size() && (isalnum((unsigned char)body[i]) || body[i] == '_')) i++;
        std::string key = body.substr(k0, i - k0);
        if (i >= body.size() || body[i] != '=') {
            while (i < body.size() && body[i] != ',') i++;
            continue;
        }
        i++;
        std::string val;
        if (i < body.size() && body[i] == '"') {
            i++;
            while (i < body.size() && body[i] != '"') {
                if (body[i] == '\\' && i + 1 < body.size()) i++;
                val.push_back(body[i++]);
            }
            i++;
        } else {
            while (i < body.size() && body[i] != ',') val.push_back(body[i++]);
            val = util::trim(val);
        }
        if (!key.empty()) m[key] = val;
    }
    return m;
}

static std::string digest_header(std::map<std::string, std::string>& ch, const std::string& user, const std::string& pass,
                                 const std::string& method, const std::string& uri) {
    std::string realm = ch["realm"], nonce = ch["nonce"], qop = ch["qop"], opaque = ch["opaque"];
    std::string algorithm = ch["algorithm"].empty() ? "MD5" : ch["algorithm"];
    std::string ha1 = util::md5_hex(user + ":" + realm + ":" + pass);
    std::string ha2 = util::md5_hex(method + ":" + uri);
    std::string nc = "00000001";
    std::string cnonce = util::new_id(16);
    std::string resp = qop.empty() ? util::md5_hex(ha1 + ":" + nonce + ":" + ha2)
                                   : util::md5_hex(ha1 + ":" + nonce + ":" + nc + ":" + cnonce + ":" + qop + ":" + ha2);
    std::string h = "Digest username=\"" + user + "\", realm=\"" + realm + "\", nonce=\"" + nonce + "\", uri=\"" + uri +
                    "\", response=\"" + resp + "\"";
    if (!qop.empty()) h += ", qop=" + qop + ", nc=" + nc + ", cnonce=\"" + cnonce + "\"";
    if (!opaque.empty()) h += ", opaque=\"" + opaque + "\"";
    h += ", algorithm=" + algorithm;
    return h;
}

struct USlot {
    std::string pid, name;
    plat::Conn* conn = nullptr;
    std::string epilogue;
    size_t size = 0;
    uint32_t created = 0;
    bool busy = false;
};
static std::map<std::string, std::shared_ptr<USlot>>* g_slots = nullptr;

static void ensure() {
    if (!g_m) g_m = new plat::Mutex();
    if (!g_pending) g_pending = new std::map<std::string, Pending>();
    if (!g_slots) g_slots = new std::map<std::string, std::shared_ptr<USlot>>();
}

std::string upload_begin(const std::string& pid, const std::string& filename, size_t size, std::string* err) {
    ensure();
    housekeeping();
    if (!g_slots) g_slots = new std::map<std::string, std::shared_ptr<USlot>>();
    std::string ip, auth_id, auth_key;
    uint16_t port = 80;
    {
        plat::Lock lk(cfg::mtx());
        JsonObject p = cfg::find_printer(pid);
        if (p.isNull()) {
            if (err) *err = "Drucker nicht gefunden.";
            return "";
        }
        if (std::string(p["type"] | "") != "ultimaker") {
            if (err) *err = "Diese Funktion ist nur fuer Ultimaker-Drucker verfuegbar.";
            return "";
        }
        ip = p["ip"] | "";
        port = (uint16_t)(p["port"] | 80);
        auth_id = p["ultimaker_auth_id"] | "";
        auth_key = p["ultimaker_auth_key"] | "";
    }
    if (auth_id.empty() || auth_key.empty()) {
        if (err) *err = "Dieser Ultimaker ist noch nicht mit dem Dashboard gekoppelt - bitte zuerst koppeln.";
        return "";
    }
    // _digest_challenge_or_none(): leere POST-Anfrage -> 401 mit Challenge?
    httpc::Response pr;
    if (!httpc::request(ip, port, false, "POST", "/api/v1/print_job", {{"Accept", "application/json"}}, "", pr, 8000)) {
        if (err) *err = "Drucker nicht erreichbar: " + pr.error;
        return "";
    }
    std::map<std::string, std::string> ch;
    bool use_digest = false;
    if (pr.status == 401) {
        std::string wa = pr.header("www-authenticate");
        if (!util::starts_with(util::lower(wa), "digest")) {
            if (err) *err = "Unerwartetes Authentifizierungsschema vom Drucker: '" + wa + "'";
            return "";
        }
        ch = parse_digest(wa);
        use_digest = true;
    }
    std::string boundary = util::new_id(32);
    std::string pre = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"jobname\"\r\n\r\n" + filename + "\r\n";
    pre += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"" + filename +
           "\"\r\nContent-Type: application/octet-stream\r\n\r\n";
    std::string epi = "\r\n--" + boundary + "--\r\n";
    std::string e;
    plat::Conn* c = plat::tcp_connect(ip, port, 8000, &e);
    if (!c) {
        if (err) *err = "Drucker nicht erreichbar: " + e;
        return "";
    }
    std::string uri = "/api/v1/print_job";
    std::string req = "POST " + uri + " HTTP/1.1\r\nHost: " + ip + (port == 80 ? "" : util::fmt(":%u", port)) + "\r\n";
    req += "Connection: close\r\nAccept: application/json\r\nUser-Agent: DruckerDashboard-RP2350\r\n";
    req += "Content-Type: multipart/form-data; boundary=" + boundary + "\r\n";
    req += util::fmt("Content-Length: %u\r\n", (unsigned)(pre.size() + size + epi.size()));
    if (use_digest) req += "Authorization: " + digest_header(ch, auth_id, auth_key, "POST", uri) + "\r\n";
    req += "\r\n" + pre;
    if (!c->write_str(req)) {
        delete c;
        if (err) *err = "Senden an den Drucker fehlgeschlagen.";
        return "";
    }
    auto s = std::make_shared<USlot>();
    s->pid = pid;
    s->name = filename;
    s->conn = c;
    s->epilogue = epi;
    s->size = size;
    s->created = plat::millis();
    std::string sid = util::new_id(16);
    plat::Lock lk(*g_m);
    (*g_slots)[sid] = s;
    return sid;
}

bool upload_finish(const std::string& sid, plat::Conn* src, size_t size, std::string* err) {
    ensure();
    std::shared_ptr<USlot> s;
    {
        plat::Lock lk(*g_m);
        if (!g_slots || !g_slots->count(sid) || (*g_slots)[sid]->busy) {
            if (err) *err = "Upload-Vorbereitung nicht gefunden oder abgelaufen - bitte erneut versuchen.";
            return false;
        }
        s = (*g_slots)[sid];
        s->busy = true;
    }
    size_t total = s->size ? s->size : size;
    size_t sent = 0;
    uint8_t buf[2048];
    bool ok = true;
    while (sent < total) {
        size_t want = total - sent < sizeof(buf) ? total - sent : sizeof(buf);
        int n = src->read(buf, want, 30000);
        if (n <= 0) { ok = false; if (err) *err = "Datei-Upload vom Browser abgebrochen."; break; }
        if (!s->conn->write_all(buf, (size_t)n, 60000)) { ok = false; if (err) *err = "Verbindung zum Drucker abgebrochen."; break; }
        sent += (size_t)n;
    }
    if (ok && !s->conn->write_str(s->epilogue)) { ok = false; if (err) *err = "Verbindung zum Drucker abgebrochen."; }
    if (ok) {
        httpc::Response r;
        if (!httpc::read_response_head(s->conn, r, 60000)) {
            ok = false;
            if (err) *err = "Keine Antwort vom Drucker: " + r.error;
        } else {
            httpc::read_body(s->conn, r, 4096, 5000);
            if (r.status == 401) {
                ok = false;
                if (err) *err = "Der Drucker hat die gespeicherten Zugangsdaten abgelehnt (HTTP 401) - die Kopplung wurde "
                                "vermutlich am Drucker zurueckgesetzt. Bitte erneut koppeln.";
            } else if (r.status >= 400) {
                ok = false;
                if (err) *err = util::fmt("Drucker lehnte den Druckauftrag ab (HTTP %d)%s%s.", r.status,
                                          r.body.empty() ? "" : ": ", r.body.substr(0, 300).c_str());
            }
        }
    }
    delete s->conn;
    s->conn = nullptr;
    {
        plat::Lock lk(*g_m);
        g_slots->erase(sid);
    }
    if (ok) logf("[MK6] Ultimaker-Druckauftrag '%s' gesendet.", s->name.c_str());
    else logf("[MK6] Ultimaker-Druckauftrag '%s' fehlgeschlagen: %s", s->name.c_str(), err ? err->c_str() : "");
    return ok;
}

void upload_cancel(const std::string& sid) {
    ensure();
    std::shared_ptr<USlot> s;
    {
        plat::Lock lk(*g_m);
        if (!g_slots || !g_slots->count(sid) || (*g_slots)[sid]->busy) return;
        s = (*g_slots)[sid];
        g_slots->erase(sid);
    }
    delete s->conn;
}

void housekeeping() {
    ensure();
    if (!g_slots) return;
    std::vector<std::shared_ptr<USlot>> stale;
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
    for (auto& s : stale) delete s->conn;
}

} // namespace ultimaker
