#include "mqtt.h"
#include <string.h>

namespace mqtt {

const char* connack_text(int rc) {
    switch (rc) {
        case 0: return "Connection Accepted";
        case 1: return "Connection Refused: unacceptable protocol version";
        case 2: return "Connection Refused: identifier rejected";
        case 3: return "Connection Refused: broker unavailable";
        case 4: return "Connection Refused: bad user name or password";
        case 5: return "Connection Refused: not authorised";
        default: return "Netzwerk-/Protokollfehler";
    }
}

static void put_str(std::string& b, const std::string& s) {
    b.push_back((char)(s.size() >> 8));
    b.push_back((char)(s.size() & 0xFF));
    b += s;
}

Client::~Client() { disconnect(); }

bool Client::send_packet(uint8_t type, const std::string& body) {
    if (!conn_) return false;
    std::string pkt;
    pkt.reserve(body.size() + 5);
    pkt.push_back((char)type);
    size_t len = body.size();
    do {
        uint8_t b = len % 128;
        len /= 128;
        if (len) b |= 0x80;
        pkt.push_back((char)b);
    } while (len);
    pkt += body;
    if (!conn_->write_all((const uint8_t*)pkt.data(), pkt.size(), 10000)) {
        alive_ = false;
        last_error = "Senden fehlgeschlagen";
        return false;
    }
    last_tx_ = plat::millis();
    return true;
}

int Client::connect(plat::Conn* conn, const std::string& client_id, const std::string& user,
                    const std::string& pass, uint16_t keepalive_s, std::string* err) {
    disconnect();
    conn_ = conn;
    alive_ = true;
    keepalive_ = keepalive_s;
    std::string b;
    put_str(b, "MQTT");
    b.push_back(4);  // Protokoll 3.1.1
    uint8_t flags = 0x02; // clean session
    if (!user.empty()) flags |= 0x80;
    if (!user.empty() && !pass.empty()) flags |= 0x40;
    b.push_back((char)flags);
    b.push_back((char)(keepalive_s >> 8));
    b.push_back((char)(keepalive_s & 0xFF));
    put_str(b, client_id);
    if (!user.empty()) put_str(b, user);
    if (!user.empty() && !pass.empty()) put_str(b, pass);
    if (!send_packet(0x10, b)) {
        if (err) *err = "CONNECT konnte nicht gesendet werden";
        disconnect();
        return -1;
    }
    uint8_t hdr[4];
    if (!conn_->read_exact(hdr, 4, 10000)) {
        if (err) *err = "keine CONNACK-Antwort (Zeitueberschreitung oder Verbindung geschlossen)";
        disconnect();
        return -1;
    }
    if (hdr[0] != 0x20 || hdr[1] != 2) {
        if (err) *err = "unerwartete Antwort statt CONNACK";
        disconnect();
        return -1;
    }
    int rc = hdr[3];
    if (rc != 0) {
        if (err) *err = connack_text(rc);
        disconnect();
        return rc;
    }
    last_rx_ = plat::millis();
    ping_sent_ = 0;
    return 0;
}

bool Client::subscribe(const std::string& topic) {
    std::string b;
    uint16_t id = pkt_id_++;
    if (!pkt_id_) pkt_id_ = 1;
    b.push_back((char)(id >> 8));
    b.push_back((char)(id & 0xFF));
    put_str(b, topic);
    b.push_back(0); // QoS 0
    return send_packet(0x82, b);
}

bool Client::publish(const std::string& topic, const std::string& payload) {
    std::string b;
    put_str(b, topic);
    b += payload;
    return send_packet(0x30, b);
}

int Client::poll(Message& msg, uint32_t timeout_ms) {
    if (!conn_ || !alive_) return -1;
    uint32_t now = plat::millis();
    // Keepalive
    if (keepalive_ && now - last_tx_ > (uint32_t)keepalive_ * 1000 * 2 / 3) {
        if (!send_packet(0xC0, "")) return -1;
        if (!ping_sent_) ping_sent_ = now;
    }
    if (ping_sent_ && now - ping_sent_ > (uint32_t)keepalive_ * 1000 + 5000) {
        last_error = "Keine PINGRESP-Antwort - Verbindung gilt als verloren";
        alive_ = false;
        return -1;
    }
    uint8_t type;
    int n = conn_->read(&type, 1, timeout_ms);
    if (n == 0) return 0;
    if (n < 0) {
        last_error = "Verbindung vom Gegenueber geschlossen";
        alive_ = false;
        return -1;
    }
    // Restlaenge
    size_t len = 0, mult = 1;
    for (int i = 0; i < 4; i++) {
        uint8_t b;
        if (!conn_->read_exact(&b, 1, 10000)) {
            last_error = "Paketkopf unvollstaendig";
            alive_ = false;
            return -1;
        }
        len += (b & 0x7F) * mult;
        mult *= 128;
        if (!(b & 0x80)) break;
    }
    last_rx_ = plat::millis();
    ping_sent_ = 0;
    uint8_t ptype = type & 0xF0;
    if (ptype == 0x30 && len <= max_payload) {
        std::string body;
        body.resize(len);
        if (len && !conn_->read_exact((uint8_t*)&body[0], len, 30000)) {
            last_error = "Nachricht unvollstaendig empfangen";
            alive_ = false;
            return -1;
        }
        if (len < 2) return 0;
        size_t tl = ((uint8_t)body[0] << 8) | (uint8_t)body[1];
        if (2 + tl > len) return 0;
        size_t off = 2 + tl;
        uint8_t qos = (type >> 1) & 3;
        if (qos > 0) off += 2;  // Paket-ID (wird bei QoS-0-Abo nicht erwartet)
        if (off > len) return 0;
        msg.topic.assign(body, 2, tl);
        msg.payload.assign(body, off, std::string::npos);
        return 1;
    }
    // Alles andere (SUBACK, PINGRESP, zu grosse PUBLISH) ueberlesen
    uint8_t tmp[512];
    size_t left = len;
    while (left) {
        size_t want = left < sizeof(tmp) ? left : sizeof(tmp);
        if (!conn_->read_exact(tmp, want, 30000)) {
            alive_ = false;
            return -1;
        }
        left -= want;
    }
    return 0;
}

void Client::disconnect() {
    if (conn_) {
        if (alive_) {
            uint8_t d[2] = {0xE0, 0x00};
            conn_->write_all(d, 2, 1000);
        }
        conn_->close();
        delete conn_;
        conn_ = nullptr;
    }
    alive_ = false;
}

} // namespace mqtt
