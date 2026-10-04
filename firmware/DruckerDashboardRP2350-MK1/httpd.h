// Kleiner HTTP/1.1-Server (eine Aufgabe je Verbindung, "Connection: close")
#pragma once
#include <map>
#include <string>
#include <vector>
#include "plat.h"

namespace http {

struct Request {
    plat::Conn* conn = nullptr;
    std::string method, path, query;
    std::map<std::string, std::string> headers;   // Schluessel klein geschrieben
    long content_length = -1;
    std::string body;                              // nur wenn nicht gestreamt
    std::vector<std::string> params;               // Platzhalter aus dem Routenmuster
    bool responded = false;

    std::string header(const std::string& k) const;
    std::string qp(const std::string& k, bool* found = nullptr) const;
};

typedef void (*Handler)(Request&);

// Muster wie "/api/printers/:id/extras/:eid". stream_body: Rumpf NICHT
// vorab einlesen (Handler liest selbst von req.conn, z. B. Datei-Uploads).
void route(const char* method, const char* pattern, Handler h, bool stream_body = false);
void start(uint16_t port);

// ---- Antworten ----
void send(Request& r, int status, const char* ctype, const std::string& body, const char* extra_headers = nullptr);
void send_bytes(Request& r, int status, const char* ctype, const uint8_t* data, size_t len, const char* extra_headers = nullptr);
void send_json(Request& r, int status, const std::string& json);
void send_error(Request& r, int status, const std::string& msg);
void send_ok(Request& r);
// Kopfzeilen fuer eine Antwort ohne Laenge (Streaming bis Verbindungsende)
bool begin_stream(Request& r, int status, const char* ctype, const char* extra_headers = nullptr);
// Restlichen Anfragerumpf verwerfen (z. B. nach frueher Fehlerantwort)
void drain_body(Request& r, size_t already_read, uint32_t timeout_ms = 30000);

const char* status_text(int code);
int active_connections();

} // namespace http
