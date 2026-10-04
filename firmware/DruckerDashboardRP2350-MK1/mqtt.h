// Minimaler MQTT-3.1.1-Client (QoS 0) - ersetzt paho-mqtt aus dem Original.
// Nicht threadsicher: alle Aufrufe aus EINER Aufgabe (der Verbindungsaufgabe).
#pragma once
#include <string>
#include "plat.h"

namespace mqtt {

struct Message {
    std::string topic;
    std::string payload;
};

class Client {
public:
    ~Client();
    // conn wird uebernommen. Liefert CONNACK-Code (0 = ok), -1 = Netzwerk/Protokollfehler.
    int connect(plat::Conn* conn, const std::string& client_id, const std::string& user,
                const std::string& pass, uint16_t keepalive_s, std::string* err);
    bool subscribe(const std::string& topic);
    bool publish(const std::string& topic, const std::string& payload);
    // Wartet bis timeout_ms auf ein Paket. 1 = PUBLISH in msg, 0 = nichts/anderes Paket,
    // -1 = Verbindung verloren. Sendet bei Bedarf selbststaendig PINGREQ.
    int poll(Message& msg, uint32_t timeout_ms);
    void disconnect();
    bool connected() const { return conn_ != nullptr && alive_; }
    std::string last_error;
    size_t max_payload = 160 * 1024;

private:
    plat::Conn* conn_ = nullptr;
    bool alive_ = false;
    uint16_t keepalive_ = 30;
    uint32_t last_tx_ = 0;
    uint32_t last_rx_ = 0;
    uint32_t ping_sent_ = 0;
    uint16_t pkt_id_ = 1;
    bool send_packet(uint8_t type, const std::string& body);
};

const char* connack_text(int rc);

} // namespace mqtt
