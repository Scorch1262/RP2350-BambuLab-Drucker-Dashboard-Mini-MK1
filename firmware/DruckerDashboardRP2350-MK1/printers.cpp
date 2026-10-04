#include "printers.h"
#include "bambu.h"
#include "pollers.h"
#include "config.h"
#include <map>

PrinterConn::PrinterConn(JsonObjectConst pcfg) {
    cfg_.set(pcfg);
    id = pcfg["id"] | "";
    name = pcfg["name"] | "";
    ip = pcfg["ip"] | "";
    type = pcfg["type"] | "bambu";
}

bool PrinterConn::sleep_stoppable(uint32_t ms) {
    uint32_t start = plat::millis();
    while (plat::millis() - start < ms) {
        if (stop_req) return true;
        plat::sleep_ms(ms < 100 ? ms : 100);
    }
    return stop_req;
}

void PrinterConn::task_entry(void* arg) {
    std::shared_ptr<PrinterConn>* sp = (std::shared_ptr<PrinterConn>*)arg;
    std::shared_ptr<PrinterConn> p = *sp;
    delete sp;
    p->run();
    // p wird hier freigegeben - ist der Drucker bereits entfernt, endet
    // damit auch das Objekt.
}

void PrinterConn::spawn(const char* tname, uint32_t stack) {
    std::shared_ptr<PrinterConn> sp = self.lock();
    if (!sp) return;
    std::shared_ptr<PrinterConn>* arg = new std::shared_ptr<PrinterConn>(sp);
    if (!plat::task_start(tname, task_entry, arg, stack, 2)) {
        logf("[SYS] Aufgabe fuer Drucker '%s' konnte nicht gestartet werden (Speicher?)", name.c_str());
        delete arg;
    }
}

namespace printers {

static plat::Mutex* g_m = nullptr;
static std::map<std::string, std::shared_ptr<PrinterConn>>* g_map = nullptr;

static void ensure() {
    if (!g_m) g_m = new plat::Mutex();
    if (!g_map) g_map = new std::map<std::string, std::shared_ptr<PrinterConn>>();
}

std::shared_ptr<PrinterConn> add_from_cfg(JsonObjectConst pcfg) {
    ensure();
    std::string t = pcfg["type"] | "bambu";
    std::shared_ptr<PrinterConn> c;
    if (cfg::is_formlabs(t)) c = std::make_shared<FormlabsConn>(pcfg);
    else if (t == "octoprint") c = std::make_shared<OctoPrintConn>(pcfg);
    else if (cfg::is_creality(t)) c = std::make_shared<CrealityConn>(pcfg);
    else if (t == "ultimaker") c = std::make_shared<UltimakerConn>(pcfg);
    else c = std::make_shared<BambuConn>(pcfg);
    c->self = c;
    {
        plat::Lock lk(*g_m);
        auto it = g_map->find(c->id);
        if (it != g_map->end()) it->second->stop();
        (*g_map)[c->id] = c;
    }
    c->start();
    return c;
}

void start_all() {
    ensure();
    JsonDocument copy;
    {
        plat::Lock lk(cfg::mtx());
        copy.set(cfg::doc()["printers"]);
    }
    for (JsonObjectConst p : copy.as<JsonArrayConst>()) add_from_cfg(p);
}

std::shared_ptr<PrinterConn> get(const std::string& id) {
    ensure();
    plat::Lock lk(*g_m);
    auto it = g_map->find(id);
    return it == g_map->end() ? nullptr : it->second;
}

void remove(const std::string& id) {
    ensure();
    std::shared_ptr<PrinterConn> c;
    {
        plat::Lock lk(*g_m);
        auto it = g_map->find(id);
        if (it == g_map->end()) return;
        c = it->second;
        g_map->erase(it);
    }
    c->stop();
}

std::vector<std::shared_ptr<PrinterConn>> all() {
    ensure();
    plat::Lock lk(*g_m);
    std::vector<std::shared_ptr<PrinterConn>> v;
    for (auto& kv : *g_map) v.push_back(kv.second);
    return v;
}

} // namespace printers
