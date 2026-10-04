// Minimaler HTTP/1.1-Client (ersetzt urllib.request im Original)
#pragma once
#include <map>
#include <string>
#include <utility>
#include <vector>
#include "plat.h"

namespace httpc {

struct Response {
    int status = 0;
    std::map<std::string, std::string> headers;   // klein geschrieben
    std::string body;
    std::string error;                            // Netzwerkfehler (status == 0)
    std::string header(const std::string& k) const;
};

typedef std::vector<std::pair<std::string, std::string>> Headers;

// Kompletter Request. Liefert false bei Netzwerkfehler (out.error gesetzt);
// HTTP-Fehlercodes (4xx/5xx) liefern true mit out.status.
bool request(const std::string& host, uint16_t port, bool use_tls, const std::string& method,
             const std::string& path, const Headers& headers, const std::string& body, Response& out,
             uint32_t timeout_ms = 6000, size_t max_body = 96 * 1024);

// Bausteine fuer Streaming-Uploads
bool read_response_head(plat::Conn* c, Response& out, uint32_t timeout_ms);
bool read_body(plat::Conn* c, Response& out, size_t max_body, uint32_t timeout_ms);

} // namespace httpc
