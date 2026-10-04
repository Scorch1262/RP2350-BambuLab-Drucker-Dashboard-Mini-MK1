// REST-API - Portierung der Flask-Routen aus app.py (MK6 v2.5.5).
// Entfernt (auf Wunsch): Druckverlauf (/history...) und Warteschlange
// (/queue...) samt Einstellung "history_max_jobs".
#include "api.h"
#include <ArduinoJson.h>
#include <algorithm>
#include <ctype.h>
#include <stdlib.h>
#include "bambu.h"
#include "camera.h"
#include "config.h"
#include "extras.h"
#include "httpd.h"
#include "pollers.h"
#include "printers.h"
#include "util.h"
#include "web_index.h"

using http::Request;

namespace api {

// ---------------------------------------------------------------------
// Hilfen
// ---------------------------------------------------------------------
static bool parse_body(Request& r, JsonDocument& d) {
    if (r.body.empty()) {
        d.to<JsonObject>();
        return true;
    }
    if (deserializeJson(d, r.body)) {
        http::send_error(r, 400, "Ungueltiges JSON im Anfragerumpf.");
        return false;
    }
    if (!d.is<JsonObject>()) d.to<JsonObject>();
    return true;
}

static std::string jstr(JsonVariantConst v) {
    if (v.is<const char*>()) return util::trim(v.as<const char*>());
    if (v.is<long>()) return util::fmt("%ld", v.as<long>());
    if (v.is<double>()) return util::fmt("%g", v.as<double>());
    return "";
}

// int(data.get("port") or default)
static bool jport(JsonVariantConst v, long def, long& out) {
    if (v.isNull()) { out = def; return true; }
    if (v.is<long>()) { out = v.as<long>(); return out != 0 || (out = def, true); }
    if (v.is<const char*>()) {
        std::string s = util::trim(v.as<const char*>());
        if (s.empty()) { out = def; return true; }
        char* end = nullptr;
        long x = strtol(s.c_str(), &end, 10);
        if (!end || *end) return false;
        out = x ? x : def;
        return true;
    }
    if (v.is<bool>()) { out = v.as<bool>() ? 1 : def; return true; }
    return false;
}

static std::string dump(JsonDocument& d) {
    std::string s;
    serializeJson(d, s);
    return s;
}

static const char* BUSY_STATES[] = {"RUNNING", "PRINTING", "WASHING", "CURING", "BUSY", "OPERATIONAL", "PAUSE", "PAUSED", nullptr};
static bool is_busy_state(const std::string& s) {
    std::string u = util::upper(s);
    for (int i = 0; BUSY_STATES[i]; i++)
        if (u == BUSY_STATES[i]) return true;
    return false;
}

// werkzeug.utils.secure_filename() (vereinfacht, ASCII)
static std::string secure_filename(const std::string& in) {
    std::string s;
    for (size_t i = 0; i < in.size(); i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '/' || c == '\\') { s.push_back(' '); continue; }
        if (c >= 0x80) continue;   // Nicht-ASCII entfaellt (Frontend normalisiert vorher NFKD)
        s.push_back((char)c);
    }
    std::string joined;
    for (auto& part : util::split(s, ' ')) {
        std::string t = util::trim(part);
        if (t.empty()) continue;
        if (!joined.empty()) joined.push_back('_');
        joined += t;
    }
    std::string out;
    for (char c : joined)
        if (isalnum((unsigned char)c) || c == '_' || c == '.' || c == '-') out.push_back(c);
    size_t a = 0;
    while (a < out.size() && (out[a] == '.' || out[a] == '_')) a++;
    size_t b = out.size();
    while (b > a && (out[b - 1] == '.' || out[b - 1] == '_')) b--;
    return out.substr(a, b - a);
}

// ---------------------------------------------------------------------
// Status (DashboardApp.all_status())
// ---------------------------------------------------------------------
static void resolve_extras(JsonArrayConst src, JsonArray out) {
    for (JsonObjectConst ex : src) {
        JsonObject item = out.add<JsonObject>();
        item.set(ex);
        if (std::string(ex["kind"] | "") == "sensor") {
            std::string v;
            if (extras::get_value(ex["topic"] | "", v)) item["value"] = v;
            else item["value"] = nullptr;
        }
    }
}

static void build_status(JsonArray out) {
    JsonDocument pc;
    {
        plat::Lock lk(cfg::mtx());
        pc.set(cfg::doc()["printers"]);
    }
    for (JsonObjectConst p : pc.as<JsonArrayConst>()) {
        JsonObject item = out.add<JsonObject>();
        std::string id = p["id"] | "";
        std::string type = p["type"] | "bambu";
        item["id"] = id;
        item["name"] = p["name"] | "";
        item["ip"] = p["ip"] | "";
        item["type"] = type;
        item["group_id"] = p["group_id"];
        item["order"] = p["order"] | 0;
        auto conn = printers::get(id);
        if (conn) conn->status_json(item);
        resolve_extras(p["extras"].as<JsonArrayConst>(), item["extras"].to<JsonArray>());
        if (type == "ultimaker") {
            std::string a = p["ultimaker_auth_id"] | "", k = p["ultimaker_auth_key"] | "";
            item["ultimaker_paired"] = !a.empty() && !k.empty();
        }
        if (type == "bambu") item["bambu_family"] = p["bambu_family"] | "x1";
        // fuer die Kamera-Anzeige im Browser
        std::string wc = p["webcam_url"] | "";
        if (!wc.empty()) item["webcam_url"] = wc;
    }
}

static void sorted_list(const char* key, JsonArray out, bool with_values) {
    JsonDocument copy;
    {
        plat::Lock lk(cfg::mtx());
        copy.set(cfg::doc()[key]);
    }
    std::vector<JsonObjectConst> v;
    for (JsonObjectConst o : copy.as<JsonArrayConst>()) v.push_back(o);
    std::stable_sort(v.begin(), v.end(), [](JsonObjectConst a, JsonObjectConst b) {
        return (a["order"] | 0) < (b["order"] | 0);
    });
    for (auto& o : v) {
        JsonObject item = out.add<JsonObject>();
        item.set(o);
        if (with_values && std::string(o["kind"] | "") == "sensor") {
            std::string val;
            if (extras::get_value(o["topic"] | "", val)) item["value"] = val;
            else item["value"] = nullptr;
        }
    }
}

static void h_index(Request& r) {
    http::send_bytes(r, 200, "text/html; charset=utf-8", INDEX_HTML_GZ, INDEX_HTML_GZ_LEN,
                     "Content-Encoding: gzip\r\nCache-Control: no-cache\r\n");
}

static void h_version(Request& r) {
    JsonDocument d;
    d["version"] = APP_VERSION;
    d["base"] = APP_BASE;
    d["platform"] = plat::platform_name();
    http::send_json(r, 200, dump(d));
}

static void h_status(Request& r) {
    JsonDocument d;
    build_status(d.to<JsonArray>());
    http::send_json(r, 200, dump(d));
}

// Neu: alles fuer refresh() in EINER Anfrage (spart Verbindungen auf dem Board)
static void h_dashboard(Request& r) {
    JsonDocument d;
    build_status(d["printers"].to<JsonArray>());
    sorted_list("groups", d["groups"].to<JsonArray>(), false);
    sorted_list("rtsp_cameras", d["cams"].to<JsonArray>(), false);
    sorted_list("standalone_extras", d["standalone_extras"].to<JsonArray>(), true);
    http::send_json(r, 200, dump(d));
}

// ---------------------------------------------------------------------
// Drucker verwalten
// ---------------------------------------------------------------------
static void h_add_printer(Request& r) {
    JsonDocument d;
    if (!parse_body(r, d)) return;
    std::string name = jstr(d["name"]), ip = jstr(d["ip"]);
    std::string type = util::lower(jstr(d["type"]));
    if (type.empty()) type = "bambu";
    if (!cfg::is_known_type(type)) return http::send_error(r, 400, "Unbekannter Druckertyp.");
    if (name.empty() || ip.empty()) return http::send_error(r, 400, "Name und IP sind Pflichtfelder.");
    if (!util::is_ipv4(ip)) return http::send_error(r, 400, "Ungueltige IP-Adresse.");

    JsonDocument np;
    np["id"] = util::new_id(10);
    np["name"] = name;
    np["type"] = type;
    np["ip"] = ip;
    np["extras"].to<JsonArray>();
    np["group_id"] = nullptr;
    long port;
    if (cfg::is_formlabs(type)) {
    } else if (type == "octoprint") {
        std::string key = jstr(d["api_key"]);
        if (key.empty()) return http::send_error(r, 400, "Fuer OctoPrint ist der API-Key ein Pflichtfeld.");
        if (!jport(d["port"], 80, port)) return http::send_error(r, 400, "Ungueltiger Port.");
        np["api_key"] = key;
        np["port"] = port;
        np["https"] = d["https"].is<bool>() ? d["https"].as<bool>() : !jstr(d["https"]).empty();
        np["webcam_url"] = jstr(d["webcam_url"]);
    } else if (cfg::is_creality(type)) {
        if (!jport(d["port"], 7125, port)) return http::send_error(r, 400, "Ungueltiger Port.");
        np["api_key"] = jstr(d["api_key"]);
        np["port"] = port;
        np["webcam_url"] = jstr(d["webcam_url"]);
    } else if (type == "ultimaker") {
        if (!jport(d["port"], 80, port)) return http::send_error(r, 400, "Ungueltiger Port.");
        np["port"] = port;
        np["webcam_url"] = jstr(d["webcam_url"]);
    } else {
        std::string code = jstr(d["access_code"]), serial = jstr(d["serial"]);
        if (code.empty() || serial.empty())
            return http::send_error(r, 400, "Fuer Bambu Lab Drucker sind Access Code und Seriennummer Pflichtfelder.");
        std::string fam = util::lower(jstr(d["bambu_family"]));
        if (!cfg::is_bambu_family(fam)) fam = "x1";
        np["access_code"] = code;
        np["serial"] = serial;
        np["mqtt_port"] = 8883;
        np["camera_port"] = 6000;
        np["bambu_family"] = fam;
    }
    {
        plat::Lock lk(cfg::mtx());
        JsonArray pa = cfg::doc()["printers"];
        np["order"] = pa.size();
        pa.add(np.as<JsonObjectConst>());
    }
    cfg::mark_dirty();
    printers::add_from_cfg(np.as<JsonObjectConst>());
    http::send_json(r, 201, dump(np));
}

static void h_delete_printer(Request& r) {
    std::string id = r.params[0];
    {
        plat::Lock lk(cfg::mtx());
        JsonArray pa = cfg::doc()["printers"];
        for (size_t i = 0; i < pa.size(); i++) {
            if (id == (pa[i]["id"] | "")) {
                pa.remove(i);
                break;
            }
        }
    }
    cfg::mark_dirty();
    printers::remove(id);
    http::send_ok(r);
}

static bool reorder_list(const char* key, JsonArrayConst order) {
    plat::Lock lk(cfg::mtx());
    JsonArray list = cfg::doc()[key];
    std::vector<std::string> want, have;
    for (JsonVariantConst v : order) want.push_back(v | "");
    for (JsonObject o : list) have.push_back(o["id"] | "");
    std::vector<std::string> a = want, b = have;
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    if (a != b) return false;
    for (JsonObject o : list) {
        std::string id = o["id"] | "";
        o["order"] = (int)(std::find(want.begin(), want.end(), id) - want.begin());
    }
    return true;
}

static void h_reorder_printers(Request& r) {
    JsonDocument d;
    if (!parse_body(r, d)) return;
    if (!d["order"].is<JsonArray>() || d["order"].size() == 0)
        return http::send_error(r, 400, "order (Liste von Drucker-IDs) fehlt.");
    if (!reorder_list("printers", d["order"].as<JsonArrayConst>()))
        return http::send_error(r, 409, "Reihenfolge passt nicht zu den aktuell angelegten Druckern.");
    cfg::mark_dirty();
    http::send_ok(r);
}

// group_id: Zeichenkette oder null
static bool get_group_arg(Request& r, JsonDocument& d, std::string& gid, bool& is_null) {
    JsonVariantConst g = d["group_id"];
    if (g.isNull()) { is_null = true; return true; }
    if (!g.is<const char*>()) {
        http::send_error(r, 400, "group_id muss eine Zeichenkette oder null sein.");
        return false;
    }
    gid = g.as<const char*>();
    is_null = gid.empty();
    return true;
}

static void assign_group(Request& r, const char* list, const char* notfound_msg, int notfound_code) {
    JsonDocument d;
    if (!parse_body(r, d)) return;
    std::string gid;
    bool is_null = false;
    if (!get_group_arg(r, d, gid, is_null)) return;
    {
        plat::Lock lk(cfg::mtx());
        JsonObject o = cfg::find_by_id(list, r.params[0]);
        if (o.isNull() || (!is_null && !cfg::group_exists(gid))) {
            return http::send_error(r, notfound_code, notfound_msg);
        }
        if (is_null) o["group_id"] = nullptr;
        else o["group_id"] = gid;
    }
    cfg::mark_dirty();
    http::send_ok(r);
}

static void h_printer_group(Request& r) { assign_group(r, "printers", "Drucker oder Raum nicht gefunden.", 404); }

// ---------------------------------------------------------------------
// Raeume
// ---------------------------------------------------------------------
static void h_groups(Request& r) {
    JsonDocument d;
    sorted_list("groups", d.to<JsonArray>(), false);
    http::send_json(r, 200, dump(d));
}

static void h_add_group(Request& r) {
    JsonDocument d;
    if (!parse_body(r, d)) return;
    std::string name = jstr(d["name"]);
    if (name.empty()) return http::send_error(r, 400, "Name ist ein Pflichtfeld.");
    JsonDocument g;
    g["id"] = util::new_id(10);
    g["name"] = name;
    {
        plat::Lock lk(cfg::mtx());
        JsonArray ga = cfg::doc()["groups"];
        g["order"] = ga.size();
        ga.add(g.as<JsonObjectConst>());
    }
    cfg::mark_dirty();
    http::send_json(r, 201, dump(g));
}

static void h_rename_group(Request& r) {
    JsonDocument d;
    if (!parse_body(r, d)) return;
    std::string name = jstr(d["name"]);
    if (name.empty()) return http::send_error(r, 400, "Name ist ein Pflichtfeld.");
    bool ok;
    {
        plat::Lock lk(cfg::mtx());
        JsonObject g = cfg::find_by_id("groups", r.params[0]);
        ok = !g.isNull();
        if (ok) g["name"] = name;
    }
    if (!ok) return http::send_error(r, 404, "Raum nicht gefunden.");
    cfg::mark_dirty();
    http::send_ok(r);
}

static void h_delete_group(Request& r) {
    std::string gid = r.params[0];
    bool ok = false;
    {
        plat::Lock lk(cfg::mtx());
        JsonArray ga = cfg::doc()["groups"];
        for (size_t i = 0; i < ga.size(); i++) {
            if (gid == (ga[i]["id"] | "")) {
                ga.remove(i);
                ok = true;
                break;
            }
        }
        if (ok) {
            // Drucker, Kameras und eigenstaendige Sensoren NICHT loeschen - nur Zuordnung entfernen
            for (const char* list : {"printers", "rtsp_cameras", "standalone_extras"})
                for (JsonObject o : cfg::doc()[list].as<JsonArray>())
                    if (gid == (o["group_id"] | "")) o["group_id"] = nullptr;
        }
    }
    if (!ok) return http::send_error(r, 404, "Raum nicht gefunden.");
    cfg::mark_dirty();
    http::send_ok(r);
}

static void h_reorder_groups(Request& r) {
    JsonDocument d;
    if (!parse_body(r, d)) return;
    if (!d["order"].is<JsonArray>() || d["order"].size() == 0)
        return http::send_error(r, 400, "order (Liste von Raum-IDs) fehlt.");
    if (!reorder_list("groups", d["order"].as<JsonArrayConst>()))
        return http::send_error(r, 409, "Reihenfolge passt nicht zu den aktuell angelegten Raeumen.");
    cfg::mark_dirty();
    http::send_ok(r);
}

// ---------------------------------------------------------------------
// Externe RTSP-Kameras
// ---------------------------------------------------------------------
static void h_cams(Request& r) {
    JsonDocument d;
    sorted_list("rtsp_cameras", d.to<JsonArray>(), false);
    http::send_json(r, 200, dump(d));
}

static bool valid_rtsp(const std::string& url) {
    std::string l = util::lower(url);
    return util::starts_with(l, "rtsp://") || util::starts_with(l, "rtsps://");
}

static void h_add_cam(Request& r) {
    JsonDocument d;
    if (!parse_body(r, d)) return;
    std::string name = jstr(d["name"]), url = jstr(d["url"]), user = jstr(d["username"]);
    std::string pass = d["password"] | "";
    if (name.empty() || url.empty()) return http::send_error(r, 400, "Name und RTSP(S)-URL sind Pflichtfelder.");
    if (!valid_rtsp(url)) return http::send_error(r, 400, "Die URL muss mit rtsp:// oder rtsps:// beginnen.");
    std::string gid;
    bool is_null = false;
    if (!get_group_arg(r, d, gid, is_null)) return;
    JsonDocument c;
    c["id"] = util::new_id(10);
    c["name"] = name;
    c["url"] = url;
    c["username"] = user;
    c["password"] = pass;
    if (is_null) c["group_id"] = nullptr;
    else c["group_id"] = gid;
    {
        plat::Lock lk(cfg::mtx());
        if (!is_null && !cfg::group_exists(gid)) {
            return http::send_error(r, 404, "Raum nicht gefunden.");
        }
        JsonArray ca = cfg::doc()["rtsp_cameras"];
        c["order"] = ca.size();
        ca.add(c.as<JsonObjectConst>());
    }
    cfg::mark_dirty();
    http::send_json(r, 201, dump(c));
}

static void h_update_cam(Request& r) {
    JsonDocument d;
    if (!parse_body(r, d)) return;
    std::string name = jstr(d["name"]), url = jstr(d["url"]), user = jstr(d["username"]);
    std::string pass = d["password"] | "";
    if (name.empty() || url.empty()) return http::send_error(r, 400, "Name und RTSP(S)-URL sind Pflichtfelder.");
    if (!valid_rtsp(url)) return http::send_error(r, 400, "Die URL muss mit rtsp:// oder rtsps:// beginnen.");
    bool ok;
    {
        plat::Lock lk(cfg::mtx());
        JsonObject c = cfg::find_by_id("rtsp_cameras", r.params[0]);
        ok = !c.isNull();
        if (ok) {
            c["name"] = name;
            c["url"] = url;
            c["username"] = user;
            c["password"] = pass;
        }
    }
    if (!ok) return http::send_error(r, 404, "Kamera nicht gefunden.");
    cfg::mark_dirty();
    http::send_ok(r);
}

static bool remove_by_id(const char* list, const std::string& id) {
    plat::Lock lk(cfg::mtx());
    JsonArray a = cfg::doc()[list];
    for (size_t i = 0; i < a.size(); i++) {
        if (id == (a[i]["id"] | "")) {
            a.remove(i);
            return true;
        }
    }
    return false;
}

static void h_delete_cam(Request& r) {
    if (!remove_by_id("rtsp_cameras", r.params[0])) return http::send_error(r, 404, "Kamera nicht gefunden.");
    cfg::mark_dirty();
    http::send_ok(r);
}

static void h_cam_group(Request& r) { assign_group(r, "rtsp_cameras", "Kamera oder Raum nicht gefunden.", 404); }

static void h_cam_stream(Request& r) {
    std::string url, user, pass, name;
    {
        plat::Lock lk(cfg::mtx());
        JsonObject c = cfg::find_by_id("rtsp_cameras", r.params[0]);
        if (!c.isNull()) {
            url = c["url"] | "";
            user = c["username"] | "";
            pass = c["password"] | "";
            name = c["name"] | "";
        }
    }
    if (url.empty()) return http::send(r, 404, "text/plain; charset=utf-8", "Kamera nicht gefunden");
    camera::serve_external_rtsp(r, url, user, pass, name);
}

// Drucker-Kamera: A1 -> MJPEG; X1/P1/P2/H2/X2 -> Hinweis auf /rtsp; andere -> Weiterleitung
static void h_printer_cam(Request& r) {
    std::string type, ip, webcam, fam;
    {
        plat::Lock lk(cfg::mtx());
        JsonObject p = cfg::find_printer(r.params[0]);
        if (p.isNull()) {
            return http::send(r, 404, "text/plain; charset=utf-8", "Drucker nicht gefunden");
        }
        type = p["type"] | "bambu";
        ip = p["ip"] | "";
        webcam = p["webcam_url"] | "";
        fam = p["bambu_family"] | "x1";
    }
    if (type == "bambu") {
        auto conn = std::static_pointer_cast<BambuConn>(printers::get(r.params[0]));
        if (!conn) return http::send(r, 404, "text/plain; charset=utf-8", "Drucker nicht gefunden");
        if (fam != "a1") return camera::serve_bambu_rtsp(r, conn);
        return camera::serve_bambu_mjpeg(r, conn);
    }
    std::string target;
    if (type == "octoprint") target = webcam.empty() ? "http://" + ip + ":8080/webcam/?action=stream" : webcam;
    else if (cfg::is_creality(type)) target = webcam.empty() ? "http://" + ip + "/webcam/?action=stream" : webcam;
    else if (type == "ultimaker") target = webcam.empty() ? "http://" + ip + ":8080/?action=stream" : webcam;
    else return http::send(r, 400, "text/plain; charset=utf-8", "Kamera-Funktion ist fuer diesen Druckertyp nicht verfuegbar.");
    std::string loc = "Location: " + target + "\r\nCache-Control: no-store\r\n";
    http::send(r, 302, "text/plain", "", loc.c_str());
}

// ---------------------------------------------------------------------
// Einstellungen (RP2350: PreFormServer-Adresse, Netzwerk, Zeit)
// ---------------------------------------------------------------------
static void settings_json(JsonDocument& d) {
    plat::Lock lk(cfg::mtx());
    d["preform_server"] = cfg::doc()["preform_server"] | "";
    d["network"].set(cfg::doc()["network"]);
}

static void h_get_settings(Request& r) {
    JsonDocument d;
    settings_json(d);
    http::send_json(r, 200, dump(d));
}

static void h_put_settings(Request& r) {
    JsonDocument d;
    if (!parse_body(r, d)) return;
    bool reboot = false;
    if (!d["network"].isNull()) {
        JsonObjectConst n = d["network"];
        bool dhcp = n["dhcp"].isNull() ? true : n["dhcp"].as<bool>();
        std::string ip = jstr(n["ip"]), mask = jstr(n["netmask"]), gw = jstr(n["gateway"]), dns = jstr(n["dns"]);
        if (!dhcp) {
            if (!util::is_ipv4(ip)) return http::send_error(r, 400, "Feste IP-Adresse ist ungueltig.");
            if (!util::is_ipv4(mask)) return http::send_error(r, 400, "Subnetzmaske ist ungueltig.");
            if (!gw.empty() && !util::is_ipv4(gw)) return http::send_error(r, 400, "Gateway ist ungueltig.");
            if (!dns.empty() && !util::is_ipv4(dns)) return http::send_error(r, 400, "DNS-Server ist ungueltig.");
        }
        std::string host = jstr(n["hostname"]);
        for (char c : host)
            if (!isalnum((unsigned char)c) && c != '-')
                return http::send_error(r, 400, "Geraetename darf nur Buchstaben, Ziffern und '-' enthalten.");
        plat::Lock lk(cfg::mtx());
        JsonObject net = cfg::doc()["network"];
        auto upd = [&](const char* k, const std::string& v) {
            if (std::string(net[k] | "") != v) { net[k] = v; reboot = true; }
        };
        if ((net["dhcp"] | true) != dhcp) { net["dhcp"] = dhcp; reboot = true; }
        upd("ip", ip);
        upd("netmask", mask.empty() ? "255.255.255.0" : mask);
        upd("gateway", gw);
        upd("dns", dns);
        upd("hostname", host.empty() ? "drucker-dashboard" : host);
        upd("ntp_server", jstr(n["ntp_server"]).empty() ? "pool.ntp.org" : jstr(n["ntp_server"]));
        upd("timezone", jstr(n["timezone"]).empty() ? "CET-1CEST,M3.5.0,M10.5.0/3" : jstr(n["timezone"]));
    }
    if (!d["preform_server"].isNull()) {
        std::string pf = jstr(d["preform_server"]);
        if (!pf.empty() && !util::parse_url(pf).ok)
            return http::send_error(r, 400, "PreFormServer-Adresse bitte als http://IP:Port angeben (z. B. http://192.168.1.10:44388).");
        plat::Lock lk(cfg::mtx());
        cfg::doc()["preform_server"] = pf;
    }
    cfg::mark_dirty();
    JsonDocument out;
    settings_json(out);
    out["reboot_required"] = reboot;
    http::send_json(r, 200, dump(out));
}

// ---------------------------------------------------------------------
// MQTT-Sensoren/Schalter
// ---------------------------------------------------------------------
static void h_get_extras_mqtt(Request& r) {
    JsonDocument d;
    {
        plat::Lock lk(cfg::mtx());
        d.set(cfg::doc()["extras_mqtt"]);
    }
    d["state"] = extras::state_text();
    http::send_json(r, 200, dump(d));
}

static void h_set_extras_mqtt(Request& r) {
    JsonDocument d;
    if (!parse_body(r, d)) return;
    bool enabled = d["enabled"].is<bool>() ? d["enabled"].as<bool>() : !jstr(d["enabled"]).empty();
    std::string host = jstr(d["host"]);
    long port;
    if (!jport(d["port"], 1883, port)) return http::send_error(r, 400, "Ungueltiger Port.");
    if (enabled && host.empty()) return http::send_error(r, 400, "Broker-Adresse ist bei aktiviertem Broker ein Pflichtfeld.");
    {
        plat::Lock lk(cfg::mtx());
        JsonObject m = cfg::doc()["extras_mqtt"];
        std::string pass = (!d["password"].isNull()) ? std::string(d["password"] | "") : std::string(m["password"] | "");
        m["enabled"] = enabled;
        m["host"] = host;
        m["port"] = port;
        m["username"] = jstr(d["username"]);
        m["password"] = pass;
        m["tls"] = d["tls"].is<bool>() ? d["tls"].as<bool>() : false;
    }
    cfg::mark_dirty();
    extras::restart();
    h_get_extras_mqtt(r);
}

static void h_extras_discovered(Request& r) {
    JsonDocument d;
    extras::list_topics(d.to<JsonArray>());
    http::send_json(r, 200, dump(d));
}

// _validate_extra_fields()
static bool validate_extra(JsonDocument& in, const std::string& existing_kind, JsonDocument& entry, std::string& err) {
    std::string kind = existing_kind.empty() ? util::lower(jstr(in["kind"])) : existing_kind;
    if (kind != "sensor" && kind != "switch") { err = "'kind' muss 'sensor' oder 'switch' sein."; return false; }
    std::string label = jstr(in["label"]);
    if (label.empty()) { err = "Bezeichnung ist ein Pflichtfeld."; return false; }
    entry["label"] = label;
    entry["kind"] = kind;
    if (kind == "sensor") {
        std::string topic = jstr(in["topic"]);
        if (topic.empty()) { err = "MQTT-Topic ist bei einem Sensor ein Pflichtfeld."; return false; }
        entry["topic"] = topic;
        std::string unit = jstr(in["unit"]);
        if (!unit.empty()) entry["unit"] = unit;
        std::string disp = util::lower(jstr(in["display"]));
        if (disp != "temperature" && disp != "humidity") disp = "generic";
        entry["display"] = disp;
    } else {
        std::string ct = jstr(in["command_topic"]);
        if (ct.empty()) { err = "Befehls-Topic ist bei einem Schalter ein Pflichtfeld."; return false; }
        std::string on = in["payload_on"].is<const char*>() ? in["payload_on"].as<const char*>() : jstr(in["payload_on"]);
        std::string off = in["payload_off"].is<const char*>() ? in["payload_off"].as<const char*>() : jstr(in["payload_off"]);
        if (on.empty() || off.empty()) { err = "Payload fuer 'Ein' und 'Aus' sind bei einem Schalter Pflichtfelder."; return false; }
        entry["command_topic"] = ct;
        entry["payload_on"] = on;
        entry["payload_off"] = off;
    }
    return true;
}

static void h_add_extra(Request& r) {
    JsonDocument d, e;
    if (!parse_body(r, d)) return;
    std::string err;
    {
        plat::Lock lk(cfg::mtx());
        if (cfg::find_printer(r.params[0]).isNull()) err = "Drucker nicht gefunden.";
    }
    if (!err.empty()) return http::send_error(r, 400, err);
    if (!validate_extra(d, "", e, err)) return http::send_error(r, 400, err);
    e["id"] = util::new_id(10);
    {
        plat::Lock lk(cfg::mtx());
        JsonObject p = cfg::find_printer(r.params[0]);
        if (!p["extras"].is<JsonArray>()) p["extras"].to<JsonArray>();
        p["extras"].as<JsonArray>().add(e.as<JsonObjectConst>());
    }
    cfg::mark_dirty();
    http::send_json(r, 201, dump(e));
}

static void h_update_extra(Request& r) {
    JsonDocument d, e;
    if (!parse_body(r, d)) return;
    std::string kind, err;
    {
        plat::Lock lk(cfg::mtx());
        JsonObject p = cfg::find_printer(r.params[0]);
        if (p.isNull()) err = "Drucker nicht gefunden.";
        else {
            bool found = false;
            for (JsonObject x : p["extras"].as<JsonArray>())
                if (r.params[1] == (x["id"] | "")) { kind = x["kind"] | ""; found = true; }
            if (!found) err = "Eintrag nicht gefunden.";
        }
    }
    if (!err.empty()) return http::send_error(r, 400, err);
    if (!validate_extra(d, kind, e, err)) return http::send_error(r, 400, err);
    e["id"] = r.params[1];
    {
        plat::Lock lk(cfg::mtx());
        JsonObject p = cfg::find_printer(r.params[0]);
        for (JsonObject x : p["extras"].as<JsonArray>())
            if (r.params[1] == (x["id"] | "")) { x.clear(); x.set(e.as<JsonObjectConst>()); }
    }
    cfg::mark_dirty();
    http::send_json(r, 200, dump(e));
}

static void h_delete_extra(Request& r) {
    bool ok = false;
    {
        plat::Lock lk(cfg::mtx());
        JsonObject p = cfg::find_printer(r.params[0]);
        if (!p.isNull()) {
            JsonArray a = p["extras"];
            for (size_t i = 0; i < a.size(); i++)
                if (r.params[1] == (a[i]["id"] | "")) { a.remove(i); ok = true; break; }
        }
    }
    if (!ok) return http::send_error(r, 404, "Eintrag nicht gefunden.");
    cfg::mark_dirty();
    http::send_ok(r);
}

static void send_switch(Request& r, JsonObjectConst ex_copy, bool found) {
    JsonDocument d;
    if (!parse_body(r, d)) return;
    std::string action = util::lower(jstr(d["action"]));
    if (action != "on" && action != "off") return http::send_error(r, 400, "action muss 'on' oder 'off' sein.");
    if (!found || std::string(ex_copy["kind"] | "") != "switch") return http::send_error(r, 400, "Schalter nicht gefunden.");
    std::string topic = ex_copy["command_topic"] | "";
    std::string payload = action == "on" ? (ex_copy["payload_on"] | "") : (ex_copy["payload_off"] | "");
    if (topic.empty() || payload.empty())
        return http::send_error(r, 400, "Schalter ist in der Konfiguration nicht vollstaendig konfiguriert.");
    if (!extras::publish(topic, payload)) return http::send_error(r, 400, "MQTT-Verbindung fuer Sensoren/Schalter nicht verfuegbar.");
    http::send_ok(r);
}

static void h_extra_command(Request& r) {
    JsonDocument copy;
    bool found = false;
    {
        plat::Lock lk(cfg::mtx());
        JsonObject p = cfg::find_printer(r.params[0]);
        if (p.isNull()) {
            return http::send_error(r, 400, "Drucker nicht gefunden.");
        }
        for (JsonObject x : p["extras"].as<JsonArray>())
            if (r.params[1] == (x["id"] | "")) { copy.set(x); found = true; }
    }
    send_switch(r, copy.as<JsonObjectConst>(), found);
}

static void h_standalone(Request& r) {
    JsonDocument d;
    sorted_list("standalone_extras", d.to<JsonArray>(), true);
    http::send_json(r, 200, dump(d));
}

static void h_add_standalone(Request& r) {
    JsonDocument d, e;
    if (!parse_body(r, d)) return;
    std::string err;
    if (!validate_extra(d, "", e, err)) return http::send_error(r, 400, err);
    e["id"] = util::new_id(10);
    e["group_id"] = nullptr;
    {
        plat::Lock lk(cfg::mtx());
        JsonArray a = cfg::doc()["standalone_extras"];
        e["order"] = a.size();
        a.add(e.as<JsonObjectConst>());
    }
    cfg::mark_dirty();
    http::send_json(r, 201, dump(e));
}

static void h_update_standalone(Request& r) {
    JsonDocument d, e;
    if (!parse_body(r, d)) return;
    std::string kind, err;
    JsonDocument old;
    {
        plat::Lock lk(cfg::mtx());
        JsonObject x = cfg::find_by_id("standalone_extras", r.params[0]);
        if (x.isNull()) err = "Eintrag nicht gefunden.";
        else { kind = x["kind"] | ""; old.set(x); }
    }
    if (!err.empty()) return http::send_error(r, 400, err);
    if (!validate_extra(d, kind, e, err)) return http::send_error(r, 400, err);
    e["id"] = r.params[0];
    e["group_id"] = old["group_id"];
    e["order"] = old["order"] | 0;
    {
        plat::Lock lk(cfg::mtx());
        JsonObject x = cfg::find_by_id("standalone_extras", r.params[0]);
        if (!x.isNull()) { x.clear(); x.set(e.as<JsonObjectConst>()); }
    }
    cfg::mark_dirty();
    http::send_json(r, 200, dump(e));
}

static void h_delete_standalone(Request& r) {
    if (!remove_by_id("standalone_extras", r.params[0])) return http::send_error(r, 404, "Eintrag nicht gefunden.");
    cfg::mark_dirty();
    http::send_ok(r);
}

static void h_standalone_group(Request& r) { assign_group(r, "standalone_extras", "Eintrag oder Raum nicht gefunden.", 400); }

static void h_standalone_command(Request& r) {
    JsonDocument copy;
    bool found = false;
    {
        plat::Lock lk(cfg::mtx());
        JsonObject x = cfg::find_by_id("standalone_extras", r.params[0]);
        if (!x.isNull()) { copy.set(x); found = true; }
    }
    send_switch(r, copy.as<JsonObjectConst>(), found);
}

// ---------------------------------------------------------------------
// Drucken: Bambu (FTPS) - zweistufig
//   1) POST .../print/bambu/start  {name, size, mapping, profile}
//      -> FTPS-Verbindung, Login, PASV, STOR werden vorbereitet
//   2) POST .../print/bambu/upload?slot=...  (Dateiinhalt als Rumpf)
//      -> Daten werden direkt an den Drucker weitergereicht, danach Druckstart
// Die 3mf-Datei wird bereits im Browser ausgewertet (Filamente, AMS-Vorschlag).
// ---------------------------------------------------------------------
static void h_bambu_start(Request& r) {
    JsonDocument d;
    if (!parse_body(r, d)) return;
    std::string pid = r.params[0];
    std::string type, fam;
    {
        plat::Lock lk(cfg::mtx());
        JsonObject p = cfg::find_printer(pid);
        if (!p.isNull()) { type = p["type"] | "bambu"; fam = p["bambu_family"] | "x1"; }
    }
    if (type.empty()) return http::send_error(r, 400, "Drucker nicht gefunden.");
    if (type != "bambu")
        return http::send_error(r, 400, "Druckauftrag per Drag & Drop wird hier nur fuer Bambu Lab Drucker unterstuetzt.");
    auto conn = std::static_pointer_cast<BambuConn>(printers::get(pid));
    if (!conn) return http::send_error(r, 400, "Keine Verbindung zu diesem Drucker.");
    std::string name = secure_filename(jstr(d["name"]));
    if (!util::ends_with(util::lower(name), ".gcode.3mf"))
        return http::send_error(r, 400, "Nur fertig gesclicte .gcode.3mf-Dateien werden unterstuetzt (Export aus Bambu Studio/OrcaSlicer).");
    long size = d["size"] | 0L;
    if (size <= 0) return http::send_error(r, 400, "Dateigroesse fehlt.");
    std::string st = conn->current_state();
    if (is_busy_state(st))
        return http::send_error(r, 409, "Drucker ist beschaeftigt (Status " + st +
                                        ") - diese Version hat keine Warteschlange, bitte warten bis der aktuelle Druck fertig ist.");
    std::vector<int> mapping;
    bool has_mapping = d["mapping"].is<JsonArray>();
    if (!d["mapping"].isNull() && !has_mapping) return http::send_error(r, 400, "mapping muss eine Liste sein.");
    if (has_mapping)
        for (JsonVariantConst v : d["mapping"].as<JsonArrayConst>()) mapping.push_back(v.is<long>() ? (int)v.as<long>() : -1);
    if (has_mapping && mapping.empty()) has_mapping = false;
    std::string profile = jstr(d["profile"]);
    if (profile != "x1" && profile != "a1") profile = bambu::family_to_profile(fam);
    std::string err, code;
    std::string slot = bambu::upload_begin(conn, name, (size_t)size, profile, mapping, has_mapping, &err, &code);
    if (slot.empty()) {
        JsonDocument e;
        e["error"] = err;
        if (!code.empty()) e["code"] = code;
        e["profile"] = profile;
        return http::send_json(r, 502, dump(e));
    }
    JsonDocument o;
    o["ok"] = true;
    o["slot"] = slot;
    o["profile"] = profile;
    o["remote_name"] = name;
    http::send_json(r, 200, dump(o));
}

static void h_bambu_upload(Request& r) {
    std::string slot = r.qp("slot");
    if (r.content_length <= 0) {
        bambu::upload_cancel(slot);
        return http::send_error(r, 400, "Keine Datei erhalten.");
    }
    std::string err, result;
    if (!bambu::upload_finish(slot, r.conn, (size_t)r.content_length, &err, &result)) {
        return http::send_error(r, 502, err);
    }
    std::string out = "{\"ok\":true,\"ams\":" + result + "}";
    http::send_json(r, 200, out);
}

static void h_print_cancel(Request& r) {
    JsonDocument d;
    if (!parse_body(r, d)) return;
    std::string slot = jstr(d["slot"]);
    if (slot.empty()) slot = jstr(d["job_id"]);
    bambu::upload_cancel(slot);
    ultimaker::upload_cancel(slot);
    http::send_ok(r);
}

// ---------------------------------------------------------------------
// Ultimaker
// ---------------------------------------------------------------------
static void h_um_pair_start(Request& r) {
    std::string err;
    if (!ultimaker::pair_start(r.params[0], &err)) return http::send_error(r, 400, err);
    http::send_ok(r);
}

static void h_um_pair_status(Request& r) {
    std::string err, status;
    if (!ultimaker::pair_status(r.params[0], &status, &err)) return http::send_error(r, 400, err);
    http::send_json(r, 200, "{\"status\":\"" + status + "\"}");
}

static void h_um_start(Request& r) {
    JsonDocument d;
    if (!parse_body(r, d)) return;
    std::string name = secure_filename(jstr(d["name"]));
    if (!util::ends_with(util::lower(name), ".gcode"))
        return http::send_error(r, 400, "Nur fertig gesclicte .gcode-Dateien werden unterstuetzt (Export aus Cura).");
    long size = d["size"] | 0L;
    if (size <= 0) return http::send_error(r, 400, "Dateigroesse fehlt.");
    auto conn = printers::get(r.params[0]);
    if (conn && is_busy_state(conn->current_state()))
        return http::send_error(r, 409, "Drucker ist beschaeftigt (Status " + conn->current_state() +
                                        ") - diese Version hat keine Warteschlange, bitte warten bis der aktuelle Druck fertig ist.");
    std::string err;
    std::string slot = ultimaker::upload_begin(r.params[0], name, (size_t)size, &err);
    if (slot.empty()) return http::send_error(r, 400, err);
    http::send_json(r, 200, "{\"ok\":true,\"slot\":\"" + slot + "\"}");
}

static void h_um_upload(Request& r) {
    std::string slot = r.qp("slot");
    if (r.content_length <= 0) {
        ultimaker::upload_cancel(slot);
        return http::send_error(r, 400, "Keine Datei erhalten.");
    }
    std::string err;
    if (!ultimaker::upload_finish(slot, r.conn, (size_t)r.content_length, &err)) return http::send_error(r, 502, err);
    http::send_ok(r);
}

// ---------------------------------------------------------------------
// System: Konfiguration sichern/laden, Log, Neustart, Firmware-Update
// ---------------------------------------------------------------------
static void h_export(Request& r) {
    std::string s = cfg::export_json();
    http::send(r, 200, "application/json; charset=utf-8", s,
               "Content-Disposition: attachment; filename=\"config.json\"\r\nCache-Control: no-store\r\n");
}

static void restart_all_connections() {
    for (auto& c : printers::all()) printers::remove(c->id);
    printers::start_all();
    extras::restart();
}

static void h_import(Request& r) {
    if (r.content_length <= 0 || r.content_length > 196608) {
        http::drain_body(r, 0);
        return http::send_error(r, 400, "Datei fehlt oder ist zu gross (max. 192 KB).");
    }
    std::string text((size_t)r.content_length, '\0');
    if (!r.conn->read_exact((uint8_t*)&text[0], text.size(), 20000)) return http::send_error(r, 400, "Datei unvollstaendig empfangen.");
    std::string err, warn;
    if (!cfg::import_json(text, &err, &warn)) return http::send_error(r, 400, err);
    restart_all_connections();
    logf("[CFG] Konfiguration importiert.%s%s", warn.empty() ? "" : " Hinweise: ", warn.c_str());
    JsonDocument d;
    d["ok"] = true;
    d["warnings"] = warn;
    http::send_json(r, 200, dump(d));
}

static void h_log(Request& r) { http::send(r, 200, "text/plain; charset=utf-8", log_dump(), "Cache-Control: no-store\r\n"); }

static void h_system(Request& r) {
    JsonDocument d;
    d["version"] = APP_VERSION;
    d["base"] = APP_BASE;
    d["platform"] = plat::platform_name();
    d["uptime_s"] = plat::millis() / 1000;
    d["heap_free"] = plat::free_heap();
    d["heap_total"] = plat::total_heap();
    d["ip"] = plat::local_ip();
    d["mac"] = plat::mac_address();
    d["link"] = plat::link_up();
    d["http_active"] = http::active_connections();
    d["camera_streams"] = camera::active_streams();
    d["time"] = plat::time_hms();
    d["time_synced"] = plat::epoch() > 1700000000;
    http::send_json(r, 200, dump(d));
}

static void reboot_task(void*) {
    plat::sleep_ms(800);
    cfg::save_if_dirty();
    plat::reboot();
}

static void h_reboot(Request& r) {
    http::send_ok(r);
    plat::task_start("reboot", reboot_task, nullptr, 2048, 4);
}

static void h_ota(Request& r) {
    if (r.content_length <= 0) return http::send_error(r, 400, "Keine Firmware-Datei erhalten.");
    std::string err;
    if (!plat::ota_begin((size_t)r.content_length, &err)) {
        http::drain_body(r, 0);
        return http::send_error(r, 400, err);
    }
    uint8_t buf[2048];
    size_t left = (size_t)r.content_length;
    bool first = true;
    while (left) {
        int n = r.conn->read(buf, left < sizeof(buf) ? left : sizeof(buf), 20000);
        if (n <= 0) {
            plat::ota_abort();
            return http::send_error(r, 400, "Firmware-Upload abgebrochen.");
        }
        if (first) {
            first = false;
            // UF2-Dateien beginnen mit "UF2\n" - die gehoeren auf das BOOTSEL-Laufwerk
            if (n >= 4 && buf[0] == 'U' && buf[1] == 'F' && buf[2] == '2' && buf[3] == '\n') {
                plat::ota_abort();
                http::drain_body(r, (size_t)n);
                return http::send_error(r, 400, "Das ist eine .uf2-Datei - fuer das Update ueber die Weboberflaeche bitte die .bin-Datei verwenden.");
            }
        }
        if (!plat::ota_write(buf, (size_t)n)) {
            plat::ota_abort();
            http::drain_body(r, (size_t)(r.content_length - (long)left + n));
            return http::send_error(r, 400, "Schreiben der Firmware fehlgeschlagen.");
        }
        left -= (size_t)n;
    }
    if (!plat::ota_end(&err)) return http::send_error(r, 400, err);
    logf("[SYS] Firmware-Update empfangen - Neustart.");
    http::send_ok(r);
    plat::task_start("reboot", reboot_task, nullptr, 2048, 4);
}

static void h_favicon(Request& r) { http::send(r, 204, nullptr, ""); }

void housekeeping() {
    bambu::housekeeping();
    ultimaker::housekeeping();
}

void register_routes() {
    using http::route;
    route("GET", "/", h_index);
    route("GET", "/index.html", h_index);
    route("GET", "/favicon.ico", h_favicon);
    route("GET", "/api/version", h_version);
    route("GET", "/api/status", h_status);
    route("GET", "/api/dashboard", h_dashboard);
    route("GET", "/api/printers", h_status);
    route("POST", "/api/printers", h_add_printer);
    route("POST", "/api/printers/reorder", h_reorder_printers);
    route("DELETE", "/api/printers/:id", h_delete_printer);
    route("POST", "/api/printers/:id/group", h_printer_group);
    route("GET", "/api/groups", h_groups);
    route("POST", "/api/groups", h_add_group);
    route("POST", "/api/groups/reorder", h_reorder_groups);
    route("PUT", "/api/groups/:id", h_rename_group);
    route("DELETE", "/api/groups/:id", h_delete_group);
    route("GET", "/api/rtsp-cameras", h_cams);
    route("POST", "/api/rtsp-cameras", h_add_cam);
    route("PUT", "/api/rtsp-cameras/:id", h_update_cam);
    route("DELETE", "/api/rtsp-cameras/:id", h_delete_cam);
    route("POST", "/api/rtsp-cameras/:id/group", h_cam_group);
    route("GET", "/camera/rtsp/:id", h_cam_stream);
    route("GET", "/camera/:id", h_printer_cam);
    route("GET", "/api/settings", h_get_settings);
    route("PUT", "/api/settings", h_put_settings);
    route("POST", "/api/printers/:id/extras/:eid/command", h_extra_command);
    route("GET", "/api/extras_mqtt", h_get_extras_mqtt);
    route("POST", "/api/extras_mqtt", h_set_extras_mqtt);
    route("GET", "/api/extras_mqtt/discovered", h_extras_discovered);
    route("POST", "/api/printers/:id/extras", h_add_extra);
    route("PUT", "/api/printers/:id/extras/:eid", h_update_extra);
    route("DELETE", "/api/printers/:id/extras/:eid", h_delete_extra);
    route("GET", "/api/standalone_extras", h_standalone);
    route("POST", "/api/standalone_extras", h_add_standalone);
    route("PUT", "/api/standalone_extras/:eid", h_update_standalone);
    route("DELETE", "/api/standalone_extras/:eid", h_delete_standalone);
    route("POST", "/api/standalone_extras/:eid/group", h_standalone_group);
    route("POST", "/api/standalone_extras/:eid/command", h_standalone_command);
    route("POST", "/api/printers/:id/print/bambu/start", h_bambu_start);
    route("POST", "/api/printers/:id/print/bambu/upload", h_bambu_upload, true);
    route("POST", "/api/printers/:id/print/cancel", h_print_cancel);
    route("POST", "/api/printers/:id/ultimaker/pair/start", h_um_pair_start);
    route("GET", "/api/printers/:id/ultimaker/pair/status", h_um_pair_status);
    route("POST", "/api/printers/:id/ultimaker/print/start", h_um_start);
    route("POST", "/api/printers/:id/ultimaker/print/upload", h_um_upload, true);
    route("GET", "/api/config/export", h_export);
    route("POST", "/api/config/import", h_import, true);
    route("GET", "/api/log", h_log);
    route("GET", "/api/system", h_system);
    route("POST", "/api/system/reboot", h_reboot);
    route("POST", "/api/system/ota", h_ota, true);
}

} // namespace api
