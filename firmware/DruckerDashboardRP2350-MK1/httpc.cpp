#include "httpc.h"
#include "tls.h"
#include "util.h"
#include <stdlib.h>

namespace httpc {

std::string Response::header(const std::string& k) const {
    auto it = headers.find(util::lower(k));
    return it == headers.end() ? "" : it->second;
}

bool read_response_head(plat::Conn* c, Response& out, uint32_t timeout_ms) {
    std::string line;
    int r = c->read_line(line, 2048, timeout_ms);
    if (r != 1) {
        out.error = r == 0 ? "Zeitueberschreitung beim Warten auf Antwort" : "Verbindung ohne Antwort geschlossen";
        return false;
    }
    // "HTTP/1.1 200 OK"
    size_t sp = line.find(' ');
    if (sp == std::string::npos || !util::starts_with(line, "HTTP/")) {
        out.error = "Ungueltige HTTP-Antwort";
        return false;
    }
    out.status = atoi(line.c_str() + sp + 1);
    for (;;) {
        r = c->read_line(line, 4096, timeout_ms);
        if (r != 1) {
            out.error = "Antwortkopf unvollstaendig";
            return false;
        }
        if (line.empty()) break;
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string k = util::lower(util::trim(line.substr(0, colon)));
        std::string v = util::trim(line.substr(colon + 1));
        if (out.headers.count(k)) out.headers[k] += ", " + v;
        else out.headers[k] = v;
    }
    return true;
}

bool read_body(plat::Conn* c, Response& out, size_t max_body, uint32_t timeout_ms) {
    out.body.clear();
    std::string te = util::lower(out.header("transfer-encoding"));
    std::string cl = out.header("content-length");
    uint8_t buf[1024];
    if (te.find("chunked") != std::string::npos) {
        for (;;) {
            std::string line;
            if (c->read_line(line, 256, timeout_ms) != 1) return false;
            long n = strtol(line.c_str(), nullptr, 16);
            if (n <= 0) {
                // Trailer bis Leerzeile ueberspringen
                for (int i = 0; i < 16; i++) {
                    if (c->read_line(line, 1024, 2000) != 1 || line.empty()) break;
                }
                return true;
            }
            size_t left = (size_t)n;
            while (left) {
                size_t want = left < sizeof(buf) ? left : sizeof(buf);
                if (!c->read_exact(buf, want, timeout_ms)) return false;
                if (out.body.size() + want <= max_body) out.body.append((const char*)buf, want);
                left -= want;
            }
            c->read_line(line, 8, timeout_ms); // CRLF nach Chunk
        }
    }
    if (!cl.empty()) {
        size_t left = (size_t)atol(cl.c_str());
        while (left) {
            int n = c->read(buf, left < sizeof(buf) ? left : sizeof(buf), timeout_ms);
            if (n <= 0) return false;
            if (out.body.size() + (size_t)n <= max_body) out.body.append((const char*)buf, (size_t)n);
            left -= (size_t)n;
        }
        return true;
    }
    // bis Verbindungsende
    for (;;) {
        int n = c->read(buf, sizeof(buf), timeout_ms);
        if (n < 0) return true;
        if (n == 0) return false;
        if (out.body.size() + (size_t)n <= max_body) out.body.append((const char*)buf, (size_t)n);
    }
}

bool request(const std::string& host, uint16_t port, bool use_tls, const std::string& method,
             const std::string& path, const Headers& headers, const std::string& body, Response& out,
             uint32_t timeout_ms, size_t max_body) {
    out = Response();
    std::string err;
    plat::Conn* c = tls::connect_any(host, port, use_tls, timeout_ms, &err);
    if (!c) {
        out.error = err;
        return false;
    }
    std::string req = method + " " + path + " HTTP/1.1\r\n";
    bool default_port = (!use_tls && port == 80) || (use_tls && port == 443);
    req += "Host: " + host + (default_port ? "" : util::fmt(":%u", port)) + "\r\n";
    req += "Connection: close\r\nUser-Agent: DruckerDashboard-RP2350\r\n";
    bool has_cl = false;
    for (auto& h : headers) {
        req += h.first + ": " + h.second + "\r\n";
        if (util::iequals(h.first, "content-length")) has_cl = true;
    }
    if (!has_cl && (!body.empty() || method == "POST" || method == "PUT"))
        req += util::fmt("Content-Length: %u\r\n", (unsigned)body.size());
    req += "\r\n";
    bool ok = c->write_str(req) && (body.empty() || c->write_str(body));
    if (!ok) {
        out.error = "Senden der Anfrage fehlgeschlagen";
        delete c;
        return false;
    }
    if (!read_response_head(c, out, timeout_ms)) {
        delete c;
        return false;
    }
    if (method != "HEAD" && out.status != 204 && out.status != 304) {
        if (!read_body(c, out, max_body, timeout_ms)) {
            // unvollstaendiger Rumpf: Status ist trotzdem bekannt
            if (out.body.empty() && out.status == 200) {
                out.error = "Antwort unvollstaendig";
                delete c;
                return false;
            }
        }
    }
    delete c;
    return true;
}

} // namespace httpc
