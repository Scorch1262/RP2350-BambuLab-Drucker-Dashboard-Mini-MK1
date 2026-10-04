#include "httpd.h"
#include "util.h"
#include "config.h"
#include <ArduinoJson.h>
#include <string.h>
#include <stdlib.h>

namespace http {

struct Route {
    std::string method;
    std::vector<std::string> parts;
    Handler h;
    bool stream_body;
};

static std::vector<Route>* g_routes = nullptr;
static plat::Mutex* g_mtx = nullptr;
static int g_active = 0;
static const int MAX_ACTIVE = 6;
static const uint32_t HANDLER_STACK = 12 * 1024;   // Kamera-Relay mit TLS laeuft im Handler
static const size_t MAX_BODY = 64 * 1024;

int active_connections() {
    plat::Lock lk(*g_mtx);
    return g_active;
}

std::string Request::header(const std::string& k) const {
    auto it = headers.find(util::lower(k));
    return it == headers.end() ? "" : it->second;
}
std::string Request::qp(const std::string& k, bool* found) const { return util::query_param(query, k, found); }

void route(const char* method, const char* pattern, Handler h, bool stream_body) {
    if (!g_routes) g_routes = new std::vector<Route>();
    Route r;
    r.method = method;
    for (auto& p : util::split(pattern, '/'))
        if (!p.empty()) r.parts.push_back(p);
    r.h = h;
    r.stream_body = stream_body;
    g_routes->push_back(r);
}

const char* status_text(int code) {
    switch (code) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        default: return "OK";
    }
}

static std::string head(int status, const char* ctype, long len, const char* extra) {
    std::string h = util::fmt("HTTP/1.1 %d %s\r\n", status, status_text(status));
    if (ctype) h += std::string("Content-Type: ") + ctype + "\r\n";
    if (len >= 0) h += util::fmt("Content-Length: %ld\r\n", len);
    h += "Connection: close\r\n";
    if (extra) h += extra;
    h += "\r\n";
    return h;
}

void send_bytes(Request& r, int status, const char* ctype, const uint8_t* data, size_t len, const char* extra) {
    if (r.responded) return;
    r.responded = true;
    std::string h = head(status, ctype, (long)len, extra);
    if (!r.conn->write_str(h)) return;
    size_t off = 0;
    while (off < len) {
        size_t n = len - off > 4096 ? 4096 : len - off;
        if (!r.conn->write_all(data + off, n)) return;
        off += n;
    }
}

void send(Request& r, int status, const char* ctype, const std::string& body, const char* extra) {
    send_bytes(r, status, ctype, (const uint8_t*)body.data(), body.size(), extra);
}

void send_json(Request& r, int status, const std::string& json) {
    send(r, status, "application/json; charset=utf-8", json, "Cache-Control: no-store\r\n");
}

void send_error(Request& r, int status, const std::string& msg) {
    JsonDocument d;
    d["error"] = msg;
    std::string s;
    serializeJson(d, s);
    send_json(r, status, s);
}

void send_ok(Request& r) { send_json(r, 200, "{\"ok\":true}"); }

bool begin_stream(Request& r, int status, const char* ctype, const char* extra) {
    if (r.responded) return false;
    r.responded = true;
    return r.conn->write_str(head(status, ctype, -1, extra));
}

void drain_body(Request& r, size_t already, uint32_t timeout_ms) {
    if (r.content_length <= 0) return;
    size_t left = (size_t)r.content_length > already ? (size_t)r.content_length - already : 0;
    uint8_t buf[1024];
    uint32_t start = plat::millis();
    while (left > 0 && plat::millis() - start < timeout_ms) {
        int n = r.conn->read(buf, left < sizeof(buf) ? left : sizeof(buf), 2000);
        if (n < 0) break;
        left -= (size_t)n;
    }
}

static bool match(const Route& rt, const std::string& method, const std::vector<std::string>& parts,
                  std::vector<std::string>& params) {
    if (rt.method != method) return false;
    if (rt.parts.size() != parts.size()) return false;
    params.clear();
    for (size_t i = 0; i < parts.size(); i++) {
        if (!rt.parts[i].empty() && rt.parts[i][0] == ':') params.push_back(util::url_decode(parts[i]));
        else if (rt.parts[i] != parts[i]) return false;
    }
    return true;
}

static void handle_conn(plat::Conn* c) {
    Request req;
    req.conn = c;
    std::string line;
    if (c->read_line(line, 4096, 8000) != 1) return;
    std::vector<std::string> rl = util::split(line, ' ');
    if (rl.size() < 2) return;
    req.method = rl[0];
    std::string target = rl[1];
    size_t q = target.find('?');
    req.path = q == std::string::npos ? target : target.substr(0, q);
    req.query = q == std::string::npos ? "" : target.substr(q + 1);
    size_t total_hdr = 0;
    for (;;) {
        if (c->read_line(line, 4096, 8000) != 1) return;
        if (line.empty()) break;
        total_hdr += line.size();
        if (total_hdr > 16384) return;
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        req.headers[util::lower(util::trim(line.substr(0, colon)))] = util::trim(line.substr(colon + 1));
    }
    std::string cl = req.header("content-length");
    if (!cl.empty()) req.content_length = atol(cl.c_str());
    if (req.header("expect") == "100-continue") c->write_str("HTTP/1.1 100 Continue\r\n\r\n");

    std::vector<std::string> parts;
    for (auto& p : util::split(req.path, '/'))
        if (!p.empty()) parts.push_back(p);

    const Route* found = nullptr;
    bool path_known = false;
    for (auto& rt : *g_routes) {
        if (match(rt, req.method, parts, req.params)) {
            found = &rt;
            break;
        }
        std::vector<std::string> dummy;
        if (match(rt, rt.method, parts, dummy) && rt.method != req.method) path_known = true;
    }
    if (!found) {
        drain_body(req, 0, 3000);
        if (path_known) send_error(req, 405, "Methode nicht erlaubt.");
        else send_error(req, 404, "Nicht gefunden.");
        return;
    }
    if (!found->stream_body && req.content_length > 0) {
        if ((size_t)req.content_length > MAX_BODY) {
            drain_body(req, 0);
            send_error(req, 413, "Anfrage zu gross.");
            return;
        }
        req.body.resize((size_t)req.content_length);
        if (!c->read_exact((uint8_t*)&req.body[0], req.body.size(), 15000)) return;
    }
    found->h(req);
    if (!req.responded) send_error(req, 500, "Keine Antwort erzeugt.");
}

static void conn_task(void* arg) {
    plat::Conn* c = (plat::Conn*)arg;
    handle_conn(c);
    c->close();
    delete c;
    {
        plat::Lock lk(*g_mtx);
        g_active--;
    }
}

static plat::TcpServer* g_server = nullptr;

static void listen_task(void* arg) {
    (void)arg;
    for (;;) {
        plat::Conn* c = g_server->accept();
        if (!c) {
            plat::sleep_ms(5);
            continue;
        }
        bool ok;
        {
            plat::Lock lk(*g_mtx);
            uint32_t fh = plat::free_heap();
            ok = g_active < MAX_ACTIVE && (fh == 0 || fh > 24 * 1024);
            if (ok) g_active++;
        }
        if (!ok) {
            std::string body = "{\"error\":\"Board ausgelastet - bitte gleich erneut versuchen.\"}";
            c->write_str(head(503, "application/json", (long)body.size(), nullptr) + body, 1000);
            c->close();
            delete c;
            continue;
        }
        if (!plat::task_start("http", conn_task, c, HANDLER_STACK, 2)) {
            c->close();
            delete c;
            plat::Lock lk(*g_mtx);
            g_active--;
        }
    }
}

void start(uint16_t port) {
    if (!g_mtx) g_mtx = new plat::Mutex();
    g_server = new plat::TcpServer();
    if (!g_server->begin(port)) {
        logf("[HTTP] Port %u konnte nicht geoeffnet werden!", port);
        return;
    }
    logf("[HTTP] Web-Oberflaeche auf Port %u gestartet.", port);
    plat::task_start("httpd", listen_task, nullptr, 3072, 3);
}

} // namespace http
