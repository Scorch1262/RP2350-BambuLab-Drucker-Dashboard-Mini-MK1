// HTTP-abgefragte Druckertypen: Formlabs (PreFormServer), OctoPrint,
// Creality/Klipper (Moonraker), Ultimaker (lokale REST-API)
#pragma once
#include "printers.h"
#include "httpc.h"

class PollConn : public PrinterConn {
public:
    explicit PollConn(JsonObjectConst pcfg) : PrinterConn(pcfg) {}
    void start() override;
    std::string current_state() override;
protected:
    void run() override;
    virtual void poll_once() = 0;     // wirft keine Exceptions - setzt err_/connected_
    virtual void before_loop() {}
    virtual uint32_t interval_ms() { return 3000; }
    // gemeinsame Felder (m gesperrt)
    bool connected_ = false;
    std::string last_update_;
    std::string gcode_state_ = "UNKNOWN";
    OptNum progress_;
    std::string file_name_ = "-";
    OptNum chamber_, nozzle_, bed_, remaining_;
    bool has_err_ = false;
    std::string err_;
    void set_error(const std::string& e);
    void common_json(JsonObject o, bool with_temps);
};

class FormlabsConn : public PollConn {
public:
    explicit FormlabsConn(JsonObjectConst pcfg) : PollConn(pcfg) {}
    void status_json(JsonObject o) override;
    std::string current_state() override;
protected:
    void before_loop() override;
    void poll_once() override;
    uint32_t interval_ms() override { return 5000; }
private:
    bool get_json(const std::string& method, const std::string& path, const std::string& body, JsonDocument& out, std::string* err);
    std::string material_ = "-";
    std::string device_status_ = "UNKNOWN";
    std::string device_id_;
};

class OctoPrintConn : public PollConn {
public:
    explicit OctoPrintConn(JsonObjectConst pcfg) : PollConn(pcfg) {}
    void status_json(JsonObject o) override;
protected:
    void poll_once() override;
};

class CrealityConn : public PollConn {
public:
    explicit CrealityConn(JsonObjectConst pcfg) : PollConn(pcfg) {}
    void status_json(JsonObject o) override;
protected:
    void before_loop() override;
    void poll_once() override;
private:
    std::string chamber_obj_;
};

class UltimakerConn : public PollConn {
public:
    explicit UltimakerConn(JsonObjectConst pcfg) : PollConn(pcfg) {}
    void status_json(JsonObject o) override;
    uint16_t port();
protected:
    void poll_once() override;
};

namespace ultimaker {
// Kopplung (wie start_ultimaker_pairing()/check_ultimaker_pairing())
bool pair_start(const std::string& printer_id, std::string* err);
// status: "pending" | "authorized" | "unauthorized"
bool pair_status(const std::string& printer_id, std::string* status, std::string* err);
// Zweistufiger Upload (siehe api.cpp)
std::string upload_begin(const std::string& printer_id, const std::string& filename, size_t size, std::string* err);
bool upload_finish(const std::string& slot, plat::Conn* src, size_t size, std::string* err);
void upload_cancel(const std::string& slot);
void housekeeping();
}
