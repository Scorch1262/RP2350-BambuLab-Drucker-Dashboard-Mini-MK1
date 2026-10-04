// Bambu Lab: MQTT/TLS-Status, FTPS-Upload und Druckstart (project_file)
#pragma once
#include <deque>
#include <string>
#include <vector>
#include "printers.h"

namespace tls { class TlsConn; class Session; }

struct AmsSlot {
    std::string slot, type, color;
    int remain = -1;
};
struct AmsUnit {
    std::string id;
    bool has_h = false, has_raw = false;
    int humidity = 0, humidity_raw = 0;
};

class BambuConn : public PrinterConn {
public:
    explicit BambuConn(JsonObjectConst pcfg);
    void start() override;
    void status_json(JsonObject o) override;
    std::string current_state() override;

    std::string family, access_code, serial;
    uint16_t mqtt_port = 8883, camera_port = 6000;

    // MQTT waehrend eines FTPS-Uploads trennen (wie pause_mqtt()/resume_mqtt())
    void pause_mqtt();
    void resume_mqtt();
    bool wait_disconnected(uint32_t ms);
    bool wait_connected(uint32_t ms);
    bool is_connected();
    // Nachricht auf device/<serial>/request senden (ueber die Verbindungsaufgabe)
    bool publish_request(const std::string& payload, uint32_t timeout_ms, std::string* err);
    // "ipcam.rtsp_url": known=false solange kein Report mit dem Feld kam
    std::string rtsp_url(bool* known);
    std::vector<AmsSlot> ams_snapshot();

protected:
    void run() override;

private:
    void apply_report(JsonObjectConst p);
    void request_pushall();
    bool sleep_or_pause(uint32_t ms);

    // Status (m gesperrt)
    bool connected_ = false;
    std::string last_update_;
    std::string gcode_state_ = "UNKNOWN";
    OptNum progress_;
    std::string file_name_ = "-";
    OptNum chamber_, nozzle_, bed_, remaining_;
    std::vector<AmsSlot> ams_;
    std::vector<AmsUnit> units_;
    bool rtsp_known_ = false;
    std::string rtsp_url_;
    bool chamber_ctc_logged_ = false, chamber_missing_logged_ = false;

    std::atomic<bool> paused_{false};
    std::atomic<bool> mqtt_up_{false};
    uint32_t last_pushall_ = 0;
    uint32_t connected_at_ = 0;

    struct Out {
        std::string payload;
        std::atomic<int> state{0};   // 0 = wartet, 1 = gesendet, -1 = Fehler
    };
    std::deque<std::shared_ptr<Out>> outq_;
    plat::Mutex outq_m_;
};

// ---- FTPS-Upload + Druckstart (zweistufig, siehe api.cpp) -----------------
namespace bambu {

// Profile wie FTPS_PROFILES im Original
struct Profile {
    std::string name;    // "x1" | "a1"
    bool cap_tls12;      // TLS gedeckelt auf 1.2
    bool reuse_session;  // Datenverbindung nutzt TLS-Sitzung der Steuerverbindung
    bool skip_unwrap;    // Datenverbindung ohne close_notify schliessen
};
Profile profile_by_name(const std::string& n);
std::string family_to_profile(const std::string& family);

// Phase 1: Verbindung + Login + PASV + STOR vorbereiten. Liefert Slot-ID oder "".
// err_code: "553" bei abgelehntem Anlegen der Datei.
std::string upload_begin(std::shared_ptr<BambuConn> p, const std::string& remote_name, size_t size,
                         const std::string& profile, const std::vector<int>& mapping, bool has_mapping,
                         std::string* err, std::string* err_code);
// Phase 2: Daten aus src (HTTP-Rumpf) uebertragen, 226 abwarten, MQTT wieder
// verbinden, Druck starten. Ergebnis als JSON (ams-Zusammenfassung) in result.
bool upload_finish(const std::string& slot, plat::Conn* src, size_t size, std::string* err,
                   std::string* result_json);
void upload_cancel(const std::string& slot);
void housekeeping();   // abgelaufene Slots aufraeumen

} // namespace bambu
