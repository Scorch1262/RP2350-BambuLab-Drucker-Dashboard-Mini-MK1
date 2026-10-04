#include "camera.h"
#include "bambu.h"
#include "tls.h"
#include "util.h"
#include <map>
#include <stdlib.h>
#include <string.h>

namespace camera {

static plat::Mutex* g_m = nullptr;
static int g_active = 0;
static const int MAX_STREAMS = 2;   // RAM: je Stream eine TLS-Verbindung (~22 KB)

int active_streams() {
    if (!g_m) g_m = new plat::Mutex();
    plat::Lock lk(*g_m);
    return g_active;
}

struct StreamSlot {
    bool ok = false;
    StreamSlot() {
        if (!g_m) g_m = new plat::Mutex();
        plat::Lock lk(*g_m);
        if (g_active < MAX_STREAMS && plat::heap_ok(70 * 1024)) {
            g_active++;
            ok = true;
        }
    }
    ~StreamSlot() {
        if (!ok) return;
        plat::Lock lk(*g_m);
        g_active--;
    }
};

static void busy(http::Request& r) {
    http::send(r, 503, "text/plain; charset=utf-8",
               "Es laufen bereits zwei Kamera-Streams oder der Speicher des Boards ist knapp - mehr schafft "
               "das Board nicht gleichzeitig. Bitte ein anderes Kamera-Fenster schliessen und erneut versuchen.");
}

// =====================================================================
// A1-Serie: Port 6000 (bambu_mjpeg_generator())
// =====================================================================
static void put_le32(uint8_t* p, uint32_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}
static uint32_t get_le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

void serve_bambu_mjpeg(http::Request& r, std::shared_ptr<BambuConn> p) {
    StreamSlot slot;
    if (!slot.ok) return busy(r);
    std::string err;
    tls::Options o;
    o.tls12_only = true;
    tls::TlsConn* c = tls::TlsConn::connect(p->ip, p->camera_port, 5000, o, &err);
    if (!c) {
        http::send(r, 502, "text/plain; charset=utf-8", "Kamera nicht erreichbar: " + err);
        return;
    }
    uint8_t auth[80];
    memset(auth, 0, sizeof(auth));
    put_le32(auth, 0x40);
    put_le32(auth + 4, 0x3000);
    memcpy(auth + 16, "bblp", 4);
    size_t cl = p->access_code.size() < 32 ? p->access_code.size() : 32;
    memcpy(auth + 48, p->access_code.data(), cl);
    if (!c->write_all(auth, sizeof(auth))) {
        delete c;
        http::send(r, 502, "text/plain; charset=utf-8", "Kamera nicht erreichbar: Anmeldung konnte nicht gesendet werden.");
        return;
    }
    uint8_t hdr[16];
    // Ersten Frame-Kopf abwarten, bevor die Antwort beginnt (dann gibt es bei
    // Fehlern noch eine Klartext-Meldung statt eines kaputten Bildes)
    if (!c->read_exact(hdr, 16, 8000)) {
        delete c;
        http::send(r, 502, "text/plain; charset=utf-8",
                   "Kamera liefert kein Bild (Access Code korrekt? LAN-Modus aktiv?).");
        return;
    }
    http::begin_stream(r, 200, "multipart/x-mixed-replace; boundary=frame",
                       "Cache-Control: no-store\r\nPragma: no-cache\r\n");
    uint8_t buf[2048];
    for (;;) {
        uint32_t img_len = get_le32(hdr);
        uint32_t payload_len = get_le32(hdr + 4);
        if (img_len == 0 || img_len > 5000000) break;
        std::string ph = util::fmt("--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", (unsigned)img_len);
        if (!r.conn->write_str(ph, 10000)) break;
        uint32_t left = img_len;
        bool ok = true;
        while (left) {
            int n = c->read(buf, left < sizeof(buf) ? left : sizeof(buf), 10000);
            if (n <= 0) { ok = false; break; }
            if (!r.conn->write_all(buf, (size_t)n, 10000)) { ok = false; break; }
            left -= (uint32_t)n;
        }
        if (!ok) break;
        if (payload_len > img_len) {
            uint32_t skip = payload_len - img_len;
            while (skip) {
                int n = c->read(buf, skip < sizeof(buf) ? skip : sizeof(buf), 10000);
                if (n <= 0) { ok = false; break; }
                skip -= (uint32_t)n;
            }
            if (!ok) break;
        }
        if (!r.conn->write_str("\r\n", 10000)) break;
        if (!c->read_exact(hdr, 16, 10000)) break;
    }
    delete c;
}

// =====================================================================
// RTSP(S)-Relay
// =====================================================================
struct RtspResp {
    int status = 0;
    std::string reason;
    std::map<std::string, std::string> headers;
    std::string body;
    std::string h(const std::string& k) const {
        auto it = headers.find(util::lower(k));
        return it == headers.end() ? "" : it->second;
    }
};

class Rtsp {
public:
    plat::Conn* c = nullptr;
    std::string user, pass;
    int cseq = 1;
    std::string session;
    std::map<std::string, std::string> digest;   // Challenge
    bool basic = false;
    unsigned nc = 0;
    std::string last_err;

    ~Rtsp() { delete c; }

    std::string auth_for(const std::string& method, const std::string& uri) {
        if (!digest.empty()) {
            nc++;
            std::string realm = digest["realm"], nonce = digest["nonce"], qop = digest["qop"];
            std::string ha1 = util::md5_hex(user + ":" + realm + ":" + pass);
            std::string ha2 = util::md5_hex(method + ":" + uri);
            std::string r;
            if (qop.find("auth") != std::string::npos) {
                std::string ncs = util::fmt("%08x", nc), cnonce = util::new_id(16);
                r = util::md5_hex(ha1 + ":" + nonce + ":" + ncs + ":" + cnonce + ":auth:" + ha2);
                return "Authorization: Digest username=\"" + user + "\", realm=\"" + realm + "\", nonce=\"" + nonce +
                       "\", uri=\"" + uri + "\", response=\"" + r + "\", qop=auth, nc=" + ncs + ", cnonce=\"" + cnonce +
                       "\"" + (digest["opaque"].empty() ? "" : ", opaque=\"" + digest["opaque"] + "\"") + "\r\n";
            }
            r = util::md5_hex(ha1 + ":" + nonce + ":" + ha2);
            return "Authorization: Digest username=\"" + user + "\", realm=\"" + realm + "\", nonce=\"" + nonce +
                   "\", uri=\"" + uri + "\", response=\"" + r + "\"" +
                   (digest["opaque"].empty() ? "" : ", opaque=\"" + digest["opaque"] + "\"") + "\r\n";
        }
        if (basic) return "Authorization: Basic " + util::base64_encode(user + ":" + pass) + "\r\n";
        return "";
    }

    bool read_resp(RtspResp& out, uint32_t timeout) {
        std::string line;
        // ggf. eingeschobene RTP-Pakete ($) ueberspringen
        for (;;) {
            uint8_t b;
            if (c->read(&b, 1, timeout) != 1) { last_err = "keine Antwort von der Kamera"; return false; }
            if (b == '$') {
                uint8_t h[3];
                if (!c->read_exact(h, 3, timeout)) return false;
                size_t len = (h[1] << 8) | h[2];
                std::string skip(len, '\0');
                if (len && !c->read_exact((uint8_t*)&skip[0], len, timeout)) return false;
                continue;
            }
            c->unread(&b, 1);
            break;
        }
        if (c->read_line(line, 1024, timeout) != 1) { last_err = "keine Antwort von der Kamera"; return false; }
        if (!util::starts_with(line, "RTSP/")) { last_err = "keine RTSP-Antwort: " + line.substr(0, 60); return false; }
        size_t sp = line.find(' ');
        out.status = atoi(line.c_str() + sp + 1);
        size_t sp2 = line.find(' ', sp + 1);
        out.reason = sp2 == std::string::npos ? "" : line.substr(sp2 + 1);
        for (;;) {
            if (c->read_line(line, 2048, timeout) != 1) { last_err = "RTSP-Antwortkopf unvollstaendig"; return false; }
            if (line.empty()) break;
            size_t col = line.find(':');
            if (col == std::string::npos) continue;
            std::string k = util::lower(util::trim(line.substr(0, col)));
            std::string v = util::trim(line.substr(col + 1));
            if (out.headers.count(k)) out.headers[k] += "\n" + v;
            else out.headers[k] = v;
        }
        long cl = atol(out.h("content-length").c_str());
        if (cl > 0 && cl < 32768) {
            out.body.resize((size_t)cl);
            if (!c->read_exact((uint8_t*)&out.body[0], (size_t)cl, timeout)) return false;
        }
        return true;
    }

    bool send_req(const std::string& method, const std::string& url, const std::string& extra) {
        std::string req = method + " " + url + " RTSP/1.0\r\n";
        req += util::fmt("CSeq: %d\r\n", cseq++);
        req += "User-Agent: DruckerDashboard-RP2350\r\n";
        if (!session.empty()) req += "Session: " + session + "\r\n";
        req += auth_for(method, url);
        req += extra + "\r\n";
        return c->write_str(req, 10000);
    }

    bool request(const std::string& method, const std::string& url, const std::string& extra, RtspResp& out) {
        for (int attempt = 0; attempt < 2; attempt++) {
            out = RtspResp();
            if (!send_req(method, url, extra)) { last_err = "Senden fehlgeschlagen"; return false; }
            if (!read_resp(out, 10000)) return false;
            if (out.status == 401 && attempt == 0 && (!user.empty() || !pass.empty())) {
                std::string wa = out.h("www-authenticate");
                // mehrere Header moeglich (durch \n getrennt) - Digest bevorzugen
                bool got = false;
                for (auto& one : util::split(wa, '\n')) {
                    std::string l = util::lower(util::trim(one));
                    if (util::starts_with(l, "digest")) {
                        digest = parse_challenge(one);
                        got = true;
                        break;
                    }
                }
                if (!got && util::lower(wa).find("basic") != std::string::npos) {
                    basic = true;
                    got = true;
                }
                if (got) continue;
            }
            return true;
        }
        return true;
    }

    static std::map<std::string, std::string> parse_challenge(const std::string& hv) {
        std::map<std::string, std::string> m;
        size_t sp = hv.find(' ');
        std::string b = sp == std::string::npos ? "" : hv.substr(sp + 1);
        size_t i = 0;
        while (i < b.size()) {
            while (i < b.size() && (b[i] == ' ' || b[i] == ',')) i++;
            size_t k0 = i;
            while (i < b.size() && b[i] != '=' && b[i] != ',') i++;
            std::string key = util::lower(util::trim(b.substr(k0, i - k0)));
            if (i >= b.size() || b[i] != '=') continue;
            i++;
            std::string val;
            if (i < b.size() && b[i] == '"') {
                i++;
                while (i < b.size() && b[i] != '"') val.push_back(b[i++]);
                i++;
            } else {
                while (i < b.size() && b[i] != ',') val.push_back(b[i++]);
            }
            m[key] = util::trim(val);
        }
        return m;
    }
};

static std::string resolve(const std::string& base, const std::string& ctrl) {
    if (ctrl.empty() || ctrl == "*") return base;
    if (util::starts_with(util::lower(ctrl), "rtsp://") || util::starts_with(util::lower(ctrl), "rtsps://")) return ctrl;
    if (!base.empty() && base.back() == '/') return base + ctrl;
    return base + "/" + ctrl;
}

static void send_frame(std::string& buf, uint8_t type, const uint8_t* data, size_t len) {
    uint8_t h[5] = {type, (uint8_t)(len >> 24), (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len};
    buf.append((const char*)h, 5);
    buf.append((const char*)data, len);
}

// Gemeinsamer RTSP-Ablauf. url ohne Zugangsdaten.
static void relay(http::Request& r, const std::string& url, const std::string& user, const std::string& pass,
                  const std::string& label) {
    StreamSlot slot;
    if (!slot.ok) return busy(r);
    util::Url u = util::parse_url(url);
    if (!u.ok || (u.scheme != "rtsp" && u.scheme != "rtsps")) {
        http::send(r, 400, "text/plain; charset=utf-8", "Ungueltige RTSP-URL.");
        return;
    }
    std::string err;
    tls::Options o;
    o.tls12_only = true;
    plat::Conn* c = tls::connect_any(u.host, u.port, u.scheme == "rtsps", 6000, &err, &o);
    if (!c) {
        http::send(r, 502, "text/plain; charset=utf-8", "Kamera nicht erreichbar: " + err);
        return;
    }
    Rtsp s;
    s.c = c;
    s.user = user;
    s.pass = pass;
    // URL ohne Zugangsdaten verwenden
    std::string clean = u.scheme + "://" + u.host + util::fmt(":%u", u.port) + u.path;
    RtspResp resp;
    auto fail = [&](const std::string& step) {
        std::string msg;
        if (resp.status == 401) msg = "Anmeldung an der Kamera abgelehnt (RTSP 401) - Benutzername/Passwort bzw. Access Code pruefen.";
        else if (resp.status) msg = util::fmt("Kamera antwortet bei %s mit RTSP %d %s", step.c_str(), resp.status, resp.reason.c_str());
        else msg = "Kamera-Stream konnte nicht gestartet werden (" + step + "): " + s.last_err;
        logf("[KAMERA] (%s): %s", label.c_str(), msg.c_str());
        http::send(r, 502, "text/plain; charset=utf-8", msg);
    };
    if (!s.request("OPTIONS", clean, "", resp)) return fail("OPTIONS");
    if (!s.request("DESCRIBE", clean, "Accept: application/sdp\r\n", resp) || resp.status != 200) return fail("DESCRIBE");
    std::string base = resp.h("content-base");
    if (base.empty()) base = resp.h("content-location");
    if (base.empty()) base = clean;
    // SDP auswerten: erste Video-Spur
    std::string session_ctrl, v_ctrl, v_codec, v_fmtp;
    int v_pt = -1;
    bool in_video = false, in_media = false;
    for (auto& raw : util::split(resp.body, '\n')) {
        std::string l = util::trim(raw);
        if (util::starts_with(l, "m=")) {
            in_media = true;
            in_video = false;
            if (v_pt < 0 && util::starts_with(l, "m=video")) {
                auto f = util::split(l, ' ');
                if (f.size() >= 4) {
                    v_pt = atoi(f[3].c_str());
                    in_video = true;
                }
            }
            continue;
        }
        if (!in_media && util::starts_with(l, "a=control:")) session_ctrl = l.substr(10);
        if (!in_video) continue;
        if (util::starts_with(l, "a=control:")) v_ctrl = l.substr(10);
        else if (util::starts_with(l, "a=rtpmap:")) {
            size_t sp = l.find(' ');
            if (sp != std::string::npos) v_codec = util::upper(util::split(l.substr(sp + 1), '/')[0]);
        } else if (util::starts_with(l, "a=fmtp:")) {
            size_t sp = l.find(' ');
            if (sp != std::string::npos) v_fmtp = l.substr(sp + 1);
        }
    }
    if (v_pt < 0) {
        http::send(r, 502, "text/plain; charset=utf-8", "Die Kamera bietet keinen Video-Stream an (SDP ohne m=video).");
        return;
    }
    std::string setup_url = resolve(base, v_ctrl);
    if (!s.request("SETUP", setup_url, "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n", resp) || resp.status != 200)
        return fail("SETUP");
    std::string sess = resp.h("session");
    int sess_timeout = 60;
    size_t semi = sess.find(';');
    if (semi != std::string::npos) {
        size_t tpos = sess.find("timeout=");
        if (tpos != std::string::npos) sess_timeout = atoi(sess.c_str() + tpos + 8);
        sess = sess.substr(0, semi);
    }
    s.session = util::trim(sess);
    int rtp_channel = 0;
    std::string tr = resp.h("transport");
    size_t ip = tr.find("interleaved=");
    if (ip != std::string::npos) rtp_channel = atoi(tr.c_str() + ip + 12);
    std::string play_url = session_ctrl.empty() ? base : resolve(base, session_ctrl);
    if (!s.request("PLAY", play_url, "Range: npt=0.000-\r\n", resp) || resp.status != 200) return fail("PLAY");

    logf("[KAMERA] (%s): RTSP-Stream gestartet (Codec %s, %s).", label.c_str(), v_codec.c_str(),
         u.scheme == "rtsps" ? "RTSPS" : "RTSP");
    http::begin_stream(r, 200, "application/octet-stream", "Cache-Control: no-store\r\nX-Accel-Buffering: no\r\n");
    JsonDocument info;
    info["codec"] = v_codec;
    info["pt"] = v_pt;
    info["fmtp"] = v_fmtp;
    info["clock"] = 90000;
    std::string ij;
    serializeJson(info, ij);
    std::string out;
    send_frame(out, 1, (const uint8_t*)ij.data(), ij.size());
    if (!r.conn->write_str(out)) return;
    out.clear();

    uint32_t keepalive_ms = (uint32_t)(sess_timeout > 10 ? sess_timeout : 60) * 1000 / 2;
    if (keepalive_ms > 25000) keepalive_ms = 25000;
    uint32_t last_ka = plat::millis();
    uint32_t last_flush = plat::millis();
    std::string pkt;
    for (;;) {
        if (plat::millis() - last_ka > keepalive_ms) {
            s.send_req("OPTIONS", clean, "");
            last_ka = plat::millis();
        }
        uint8_t b;
        int n = c->read(&b, 1, out.empty() ? 5000 : 15);
        if (n == 0) {
            if (!out.empty()) {
                if (!r.conn->write_str(out, 15000)) break;
                out.clear();
                last_flush = plat::millis();
                continue;
            }
            // 5 s ohne Daten: Kamera haengt?
            static const char* m = "Kamera sendet keine Daten mehr.";
            send_frame(out, 3, (const uint8_t*)m, strlen(m));
            r.conn->write_str(out, 2000);
            break;
        }
        if (n < 0) break;
        if (b == '$') {
            uint8_t h[3];
            if (!c->read_exact(h, 3, 10000)) break;
            size_t len = (h[1] << 8) | h[2];
            pkt.resize(len);
            if (len && !c->read_exact((uint8_t*)&pkt[0], len, 10000)) break;
            if (h[0] == rtp_channel) send_frame(out, 2, (const uint8_t*)pkt.data(), len);
        } else {
            // RTSP-Antwort (Keepalive) ueberlesen
            c->unread(&b, 1);
            RtspResp ka;
            if (!s.read_resp(ka, 10000)) break;
        }
        if (out.size() > 2800 || (plat::millis() - last_flush > 40 && !out.empty())) {
            if (!r.conn->write_str(out, 15000)) break;   // Browser hat geschlossen
            out.clear();
            last_flush = plat::millis();
        }
    }
    s.send_req("TEARDOWN", play_url, "");
    logf("[KAMERA] (%s): Stream beendet.", label.c_str());
}

void serve_bambu_rtsp(http::Request& r, std::shared_ptr<BambuConn> p) {
    bool known = false;
    std::string st = p->rtsp_url(&known);
    if (known && st == "disable") {
        http::send(r, 409, "text/plain; charset=utf-8",
                   "Kamera-Livestream ist am Drucker nicht aktiviert. Bitte zusaetzlich zum Developer Mode am "
                   "Drucker-Display die separate Einstellung \"LAN Only Liveview\" (teils auch \"LAN Mode Liveview\" "
                   "genannt) aktivieren - siehe README.");
        return;
    }
    if (!known) {
        http::send(r, 503, "text/plain; charset=utf-8",
                   "Kamera-Status noch nicht bekannt (noch kein vollstaendiger MQTT-Report vom Drucker empfangen). "
                   "Bitte kurz warten und erneut versuchen.");
        return;
    }
    std::string url = "rtsps://" + p->ip + ":322/streaming/live/1";
    relay(r, url, "bblp", p->access_code, "Drucker " + p->name);
}

void serve_external_rtsp(http::Request& r, const std::string& url, const std::string& user, const std::string& pass,
                         const std::string& label) {
    // Wie build_rtsp_url_with_auth(): getrennt gespeicherte Zugangsdaten haben Vorrang
    util::Url u = util::parse_url(url);
    std::string usr = user, pw = pass;
    if (usr.empty() && pw.empty()) {
        usr = u.user;
        pw = u.pass;
    }
    relay(r, url, usr, pw, "Kamera " + label);
}

} // namespace camera
