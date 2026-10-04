#include "config.h"
#include "util.h"
#include <atomic>

namespace cfg {

static plat::Mutex* g_mtx = nullptr;
static JsonDocument* g_doc = nullptr;
static std::atomic<bool> g_dirty{false};
static std::atomic<uint32_t> g_dirty_since{0};

static const char* CONFIG_PATH = "/config.json";

bool is_formlabs(const std::string& t) { return t == "formlabs" || t == "formlabs_wash" || t == "formlabs_cure"; }
bool is_creality(const std::string& t) {
    return t == "creality_k1" || t == "creality_k1c" || t == "creality_k1max" || t == "creality_k1se" || t == "creality_other";
}
bool is_known_type(const std::string& t) {
    return t == "bambu" || is_formlabs(t) || t == "octoprint" || is_creality(t) || t == "ultimaker";
}
bool is_bambu_family(const std::string& f) {
    return f == "x1" || f == "a1" || f == "h2" || f == "p1" || f == "p2" || f == "x2";
}

plat::Mutex& mtx() { return *g_mtx; }
JsonDocument& doc() { return *g_doc; }

static void set_default(JsonObject o, const char* key, const char* v) {
    if (o[key].isNull()) o[key] = v;
}

void migrate(JsonDocument& d) {
    if (!d.is<JsonObject>()) d.to<JsonObject>();
    JsonObject root = d.as<JsonObject>();
    if (root["preform_server"].isNull()) root["preform_server"] = "";
    if (!root["extras_mqtt"].is<JsonObject>()) {
        JsonObject m = root["extras_mqtt"].to<JsonObject>();
        m["enabled"] = false;
        m["host"] = "";
        m["port"] = 1883;
        m["username"] = "";
        m["password"] = "";
        m["tls"] = false;
    }
    if (!root["groups"].is<JsonArray>()) root["groups"].to<JsonArray>();
    if (!root["rtsp_cameras"].is<JsonArray>()) root["rtsp_cameras"].to<JsonArray>();
    if (!root["standalone_extras"].is<JsonArray>()) root["standalone_extras"].to<JsonArray>();
    if (!root["printers"].is<JsonArray>()) root["printers"].to<JsonArray>();
    // Netzwerk (nur RP2350-Version)
    if (!root["network"].is<JsonObject>()) {
        JsonObject n = root["network"].to<JsonObject>();
        n["dhcp"] = true;
    }
    JsonObject net = root["network"];
    if (net["dhcp"].isNull()) net["dhcp"] = true;
    set_default(net, "ip", "");
    set_default(net, "netmask", "255.255.255.0");
    set_default(net, "gateway", "");
    set_default(net, "dns", "");
    set_default(net, "hostname", "drucker-dashboard");
    set_default(net, "ntp_server", "pool.ntp.org");
    // POSIX-Zeitzone, Standard: Deutschland (MEZ/MESZ)
    set_default(net, "timezone", "CET-1CEST,M3.5.0,M10.5.0/3");
    // MK6-spezifisch, in dieser Version ohne Funktion (kein Verlauf)
    root.remove("history_max_jobs");

    int idx = 0;
    for (JsonObject p : root["printers"].as<JsonArray>()) {
        if (!p["extras"].is<JsonArray>()) p["extras"].to<JsonArray>();
        if (p["type"].isNull()) p["type"] = "bambu";
        if (p["type"] == "bambu" && p["bambu_family"].isNull()) p["bambu_family"] = "x1";
        if (!p["group_id"].is<const char*>()) p["group_id"] = nullptr;
        if (p["order"].isNull()) p["order"] = idx;
        idx++;
    }
    idx = 0;
    for (JsonObject g : root["groups"].as<JsonArray>()) {
        if (g["order"].isNull()) g["order"] = idx;
        idx++;
    }
    idx = 0;
    for (JsonObject c : root["rtsp_cameras"].as<JsonArray>()) {
        if (c["order"].isNull()) c["order"] = idx;
        set_default(c, "username", "");
        set_default(c, "password", "");
        if (!c["group_id"].is<const char*>()) c["group_id"] = nullptr;
        idx++;
    }
    idx = 0;
    for (JsonObject e : root["standalone_extras"].as<JsonArray>()) {
        if (e["order"].isNull()) e["order"] = idx;
        if (!e["group_id"].is<const char*>()) e["group_id"] = nullptr;
        idx++;
    }
}

void init() {
    if (!g_mtx) g_mtx = new plat::Mutex();
    if (!g_doc) g_doc = new JsonDocument();
    plat::Lock lk(*g_mtx);
    std::string text;
    bool loaded = false;
    if (plat::fs_read(CONFIG_PATH, text)) {
        DeserializationError e = deserializeJson(*g_doc, text);
        if (e) {
            logf("[CFG] config.json ist beschaedigt (%s) - es wird eine leere Konfiguration verwendet. "
                 "Die alte Datei wird als config.broken.json aufbewahrt.", e.c_str());
            plat::fs_write("/config.broken.json", text);
            g_doc->clear();
        } else {
            loaded = true;
        }
    }
    migrate(*g_doc);
    if (!loaded) {
        logf("[CFG] Neue Konfiguration angelegt.");
        g_dirty = true;
        g_dirty_since = 0;
    }
}

void mark_dirty() {
    if (!g_dirty) g_dirty_since = plat::millis();
    g_dirty = true;
}
bool dirty() { return g_dirty; }

void save_if_dirty() {
    if (!g_dirty) return;
    // kurz sammeln (mehrere Aenderungen in Folge = ein Schreibvorgang)
    if (g_dirty_since && plat::millis() - g_dirty_since < 300) return;
    std::string text;
    {
        plat::Lock lk(*g_mtx);
        serializeJsonPretty(*g_doc, text);
        g_dirty = false;
    }
    if (!plat::fs_write(CONFIG_PATH, text)) {
        logf("[CFG] FEHLER: config.json konnte nicht gespeichert werden!");
        g_dirty = true;
        g_dirty_since = plat::millis();
    }
}

JsonObject find_by_id(const char* list, const std::string& id) {
    for (JsonObject o : (*g_doc)[list].as<JsonArray>()) {
        const char* oid = o["id"] | "";
        if (id == oid) return o;
    }
    return JsonObject();
}

JsonObject find_printer(const std::string& id) { return find_by_id("printers", id); }

bool group_exists(const std::string& id) { return !find_by_id("groups", id).isNull(); }

std::string export_json() {
    plat::Lock lk(*g_mtx);
    std::string out;
    serializeJsonPretty(*g_doc, out);
    return out;
}

bool import_json(const std::string& text, std::string* err, std::string* warnings) {
    JsonDocument nd;
    DeserializationError e = deserializeJson(nd, text);
    if (e) {
        if (err) *err = std::string("Datei ist kein gueltiges JSON: ") + e.c_str();
        return false;
    }
    if (!nd.is<JsonObject>() || !nd["printers"].is<JsonArray>()) {
        if (err) *err = "Datei sieht nicht wie eine config.json des Drucker-Dashboards aus (Feld \"printers\" fehlt).";
        return false;
    }
    std::string warn;
    // Netzwerkeinstellungen des Boards NICHT aus einer PC-config.json uebernehmen
    JsonDocument old_net;
    {
        plat::Lock lk(*g_mtx);
        old_net.set((*g_doc)["network"]);
    }
    if (nd["network"].isNull()) nd["network"].set(old_net.as<JsonVariant>());
    // ungueltige Druckertypen aussortieren
    JsonArray printers = nd["printers"].as<JsonArray>();
    for (size_t i = 0; i < printers.size();) {
        JsonObject p = printers[i];
        std::string t = p["type"] | "bambu";
        if (!is_known_type(t) || !(p["id"].is<const char*>())) {
            warn += "Drucker '" + std::string(p["name"] | "?") + "' uebersprungen (unbekannter Typ oder keine ID). ";
            printers.remove(i);
            continue;
        }
        i++;
    }
    std::string pf = nd["preform_server"] | "";
    if (pf.find("localhost") != std::string::npos || pf.find("127.0.0.1") != std::string::npos) {
        warn += "PreFormServer-Adresse '" + pf + "' zeigt auf 'localhost' - auf dem Board muss dort die IP des PCs "
                "stehen, auf dem PreFormServer laeuft (Einstellungen -> Allgemein). ";
    }
    if (!nd["history_max_jobs"].isNull()) warn += "'history_max_jobs' wird ignoriert (kein Druckverlauf in dieser Version). ";
    migrate(nd);
    {
        plat::Lock lk(*g_mtx);
        g_doc->set(nd.as<JsonVariant>());
    }
    mark_dirty();
    if (warnings) *warnings = warn;
    return true;
}

} // namespace cfg
