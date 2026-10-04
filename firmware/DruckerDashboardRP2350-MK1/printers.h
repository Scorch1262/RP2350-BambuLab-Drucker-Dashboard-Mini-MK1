// Verwaltung aller Drucker-Verbindungen (entspricht DashboardApp.connections)
#pragma once
#include <ArduinoJson.h>
#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include "plat.h"

// Optionaler Zahlenwert (Python: None oder Zahl)
struct OptNum {
    bool has = false;
    double v = 0;
    void set(double x) { has = true; v = x; }
    void clear() { has = false; }
    void put(JsonObject o, const char* key) const {
        if (has) o[key] = v;
        else o[key] = nullptr;
    }
};

class PrinterConn {
public:
    explicit PrinterConn(JsonObjectConst pcfg);
    virtual ~PrinterConn() {}
    std::string id, name, ip, type;
    std::atomic<bool> stop_req{false};
    plat::Mutex m;   // schuetzt die Statusfelder der Unterklassen

    virtual void start() = 0;
    virtual void stop() { stop_req = true; }
    // Statusfelder in o eintragen (wie conn.status im Original)
    virtual void status_json(JsonObject o) = 0;
    virtual std::string current_state() = 0;

protected:
    JsonDocument cfg_;   // Kopie des Konfigurationseintrags beim Start
    // Startet run_task() in einer eigenen Aufgabe (haelt shared_ptr am Leben)
    void spawn(const char* name, uint32_t stack);
    virtual void run() = 0;
    // Schlaeft in kleinen Schritten, bricht bei stop_req ab. true = gestoppt
    bool sleep_stoppable(uint32_t ms);
    static void task_entry(void* arg);
public:
    std::weak_ptr<PrinterConn> self;
};

namespace printers {

void start_all();                                       // beim Start aus der Konfiguration
std::shared_ptr<PrinterConn> add_from_cfg(JsonObjectConst pcfg);
std::shared_ptr<PrinterConn> get(const std::string& id);
void remove(const std::string& id);
std::vector<std::shared_ptr<PrinterConn>> all();

} // namespace printers
