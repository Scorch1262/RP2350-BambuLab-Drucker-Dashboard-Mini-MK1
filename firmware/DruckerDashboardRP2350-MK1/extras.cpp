// Portierung von ExtrasMqttManager (app.py)
#include "extras.h"
#include "config.h"
#include "mqtt.h"
#include "tls.h"
#include "util.h"
#include <atomic>
#include <deque>
#include <map>
#include <memory>

namespace extras {

static const size_t MAX_TOPICS = 400;       // Schutz vor "geschwaetzigen" Brokern
static const size_t MAX_VALUE_LEN = 200;

struct Instance {
    std::atomic<bool> stop{false};
    std::string host, user, pass;
    uint16_t port = 1883;
    bool tls = false;
    std::atomic<bool> up{false};
    std::string state;
    plat::Mutex qm;
    std::deque<std::pair<std::string, std::string>> outq;
};

static plat::Mutex* g_m = nullptr;               // Werte + aktive Instanz
static std::map<std::string, std::string>* g_values = nullptr;
static std::shared_ptr<Instance> g_inst;
static std::string g_state = "deaktiviert";

static void ensure() {
    if (!g_m) g_m = new plat::Mutex();
    if (!g_values) g_values = new std::map<std::string, std::string>();
}

static void set_state(const std::shared_ptr<Instance>& in, const std::string& s) {
    plat::Lock lk(*g_m);
    if (g_inst == in) g_state = s;
}

static void task(void* arg) {
    std::shared_ptr<Instance>* spp = (std::shared_ptr<Instance>*)arg;
    std::shared_ptr<Instance> in = *spp;
    delete spp;
    while (!in->stop) {
        std::string err;
        set_state(in, "verbinde mit " + in->host + util::fmt(":%u ...", in->port));
        plat::Conn* c = tls::connect_any(in->host, in->port, in->tls, 8000, &err);
        if (!c) {
            set_state(in, "nicht verbunden: " + err);
            for (int i = 0; i < 50 && !in->stop; i++) plat::sleep_ms(100);
            continue;
        }
        mqtt::Client cl;
        cl.max_payload = 4096;
        std::string cid = "dashboard-extras-" + util::new_id(6);
        int rc = cl.connect(c, cid, in->user, in->pass, 30, &err);
        if (rc != 0) {
            set_state(in, rc > 0 ? std::string("abgelehnt: ") + mqtt::connack_text(rc) : "nicht verbunden: " + err);
            logf("[EXTRAS-MQTT] Verbindung zu %s fehlgeschlagen: %s", in->host.c_str(),
                 rc > 0 ? mqtt::connack_text(rc) : err.c_str());
            for (int i = 0; i < 50 && !in->stop; i++) plat::sleep_ms(100);
            continue;
        }
        cl.subscribe("#");
        in->up = true;
        set_state(in, "verbunden mit " + in->host + util::fmt(":%u", in->port));
        logf("[EXTRAS-MQTT] Verbunden mit %s:%u, Abo '#'.", in->host.c_str(), in->port);
        while (!in->stop) {
            for (;;) {
                std::pair<std::string, std::string> o;
                {
                    plat::Lock lk(in->qm);
                    if (in->outq.empty()) break;
                    o = in->outq.front();
                    in->outq.pop_front();
                }
                cl.publish(o.first, o.second);
            }
            mqtt::Message msg;
            int r = cl.poll(msg, 150);
            if (r < 0) break;
            if (r == 1) {
                if (msg.payload.size() > MAX_VALUE_LEN) msg.payload = msg.payload.substr(0, MAX_VALUE_LEN);
                plat::Lock lk(*g_m);
                if (g_inst != in) break;
                auto it = g_values->find(msg.topic);
                if (it != g_values->end()) it->second = msg.payload;
                else if (g_values->size() < MAX_TOPICS) (*g_values)[msg.topic] = msg.payload;
            }
        }
        in->up = false;
        cl.disconnect();
        if (!in->stop) {
            set_state(in, "Verbindung verloren - neuer Versuch ...");
            for (int i = 0; i < 30 && !in->stop; i++) plat::sleep_ms(100);
        }
    }
}

void restart() {
    ensure();
    auto in = std::make_shared<Instance>();
    bool enabled;
    {
        plat::Lock lk(cfg::mtx());
        JsonObject m = cfg::doc()["extras_mqtt"];
        enabled = m["enabled"] | false;
        in->host = m["host"] | "";
        in->port = (uint16_t)(m["port"] | 1883);
        in->user = m["username"] | "";
        in->pass = m["password"] | "";
        in->tls = m["tls"] | false;
    }
    std::shared_ptr<Instance> old;
    {
        plat::Lock lk(*g_m);
        old = g_inst;
        g_inst = nullptr;
        g_values->clear();
        g_state = "deaktiviert";
    }
    if (old) old->stop = true;
    if (!enabled || in->host.empty()) return;
    {
        plat::Lock lk(*g_m);
        g_inst = in;
    }
    auto* arg = new std::shared_ptr<Instance>(in);
    if (!plat::task_start("extras", task, arg, 9 * 1024, 2)) {
        delete arg;
        set_state(in, "Aufgabe konnte nicht gestartet werden (Speicher?)");
    }
}

bool get_value(const std::string& topic, std::string& out) {
    ensure();
    if (topic.empty()) return false;
    plat::Lock lk(*g_m);
    auto it = g_values->find(topic);
    if (it == g_values->end()) return false;
    out = it->second;
    return true;
}

void list_topics(JsonArray out, size_t limit) {
    ensure();
    plat::Lock lk(*g_m);
    size_t n = 0;
    for (auto& kv : *g_values) {   // std::map ist bereits alphabetisch sortiert
        if (n++ >= limit) break;
        JsonObject o = out.add<JsonObject>();
        o["topic"] = kv.first;
        o["value"] = kv.second;
    }
}

bool publish(const std::string& topic, const std::string& payload) {
    ensure();
    std::shared_ptr<Instance> in;
    {
        plat::Lock lk(*g_m);
        in = g_inst;
    }
    if (!in || !in->up) return false;
    plat::Lock lk(in->qm);
    in->outq.push_back({topic, payload});
    return true;
}

std::string state_text() {
    ensure();
    plat::Lock lk(*g_m);
    return g_state;
}

} // namespace extras
