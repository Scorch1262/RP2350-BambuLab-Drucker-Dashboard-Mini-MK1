// TLS-Client auf Basis von mbedTLS (ersetzt Pythons ssl._create_unverified_context()).
#pragma once
#include "plat.h"
#include <string>

struct mbedtls_ssl_session;

namespace tls {

void global_init();   // einmal beim Start (Mutexe, PSA)

struct Options {
    bool allow_tls13 = false;      // Standard: nur TLS 1.2 (wie bewaehrte ESP32-Projekte)
    bool tls12_only = true;
    uint32_t handshake_timeout_ms = 12000;
    bool send_close_notify = true; // beim Schliessen ein TLS close_notify senden
    std::string sni;               // optional (nur bei Hostnamen sinnvoll)
};

// Gespeicherte TLS-Sitzung (fuer FTPS: Datenverbindung muss die Sitzung
// der Steuerverbindung wiederverwenden, siehe Profil "x1").
class Session {
public:
    Session();
    ~Session();
    mbedtls_ssl_session* get() { return s_; }
    bool valid = false;
private:
    mbedtls_ssl_session* s_;
};

class TlsConn : public plat::Conn {
public:
    ~TlsConn() override;
    int read_raw(uint8_t* buf, size_t len, uint32_t timeout_ms) override;
    bool write_all(const uint8_t* buf, size_t len, uint32_t timeout_ms = 15000) override;
    void close() override;
    bool is_open() override;
    int available() override;
    std::string peer() override;
    // Sitzung der bestehenden Verbindung sichern (fuer Wiederverwendung)
    bool save_session(Session& out);
    std::string version();
    std::string cipher();
    // Ohne close_notify schliessen (FTPS-Profil "a1", siehe ftps.cpp)
    void set_close_notify(bool v) { opts_.send_close_notify = v; }

    // Baut TCP + TLS auf. reuse: optionale Sitzung (wird vor dem Handshake gesetzt).
    static TlsConn* connect(const std::string& host, uint16_t port, uint32_t timeout_ms,
                            const Options& opts, std::string* err, Session* reuse = nullptr);
    // TLS ueber eine bereits bestehende TCP-Verbindung (uebernimmt Besitz)
    static TlsConn* wrap(plat::Conn* tcp, const Options& opts, std::string* err, Session* reuse = nullptr);

    struct Impl;   // intern (tls.cpp)
private:
    TlsConn();
    Impl* d_;
    Options opts_;
    bool open_ = false;
};

// Einheitlicher Verbindungsaufbau: TLS oder reines TCP
plat::Conn* connect_any(const std::string& host, uint16_t port, bool use_tls, uint32_t timeout_ms,
                        std::string* err, const Options* opts = nullptr);

std::string mbed_err(int code);

} // namespace tls
