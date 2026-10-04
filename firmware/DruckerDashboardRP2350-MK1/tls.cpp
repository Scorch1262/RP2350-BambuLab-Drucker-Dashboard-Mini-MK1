// TLS-Client (mbedTLS) - siehe tls.h
#include "tls.h"
#include <string.h>
#include <stdio.h>

#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/error.h"
#include "mbedtls/threading.h"
#include "mbedtls/platform_time.h"
#include "psa/crypto.h"

// ---- Hardware-Zufall und Zeit fuer mbedTLS ---------------------------
extern "C" int mbedtls_hardware_poll(void* data, unsigned char* output, size_t len, size_t* olen) {
    (void)data;
    size_t i = 0;
    while (i < len) {
        uint32_t r = plat::random32();
        for (int b = 0; b < 4 && i < len; b++) {
            output[i++] = (unsigned char)(r & 0xFF);
            r >>= 8;
        }
    }
    *olen = len;
    return 0;
}

extern "C" mbedtls_ms_time_t mbedtls_ms_time(void) {
    static uint32_t last = 0;
    static int64_t high = 0;
    uint32_t now = plat::millis();
    if (now < last) high += (int64_t)1 << 32;
    last = now;
    return (mbedtls_ms_time_t)(high + now);
}

// ---- Mutexe fuer mbedTLS (PSA-Schluesselspeicher ist global) ---------
static void mtx_init(mbedtls_threading_mutex_t* m) {
    m->h = new plat::Mutex();
    m->is_valid = 1;
}
static void mtx_free(mbedtls_threading_mutex_t* m) {
    if (m->is_valid) delete (plat::Mutex*)m->h;
    m->h = nullptr;
    m->is_valid = 0;
}
static int mtx_lock(mbedtls_threading_mutex_t* m) {
    if (!m->is_valid) return MBEDTLS_ERR_THREADING_BAD_INPUT_DATA;
    ((plat::Mutex*)m->h)->lock();
    return 0;
}
static int mtx_unlock(mbedtls_threading_mutex_t* m) {
    if (!m->is_valid) return MBEDTLS_ERR_THREADING_BAD_INPUT_DATA;
    ((plat::Mutex*)m->h)->unlock();
    return 0;
}

namespace tls {

static bool g_inited = false;

void global_init() {
    if (g_inited) return;
    mbedtls_threading_set_alt(mtx_init, mtx_free, mtx_lock, mtx_unlock);
    psa_status_t st = psa_crypto_init();
    if (st != PSA_SUCCESS) logf("[TLS] psa_crypto_init fehlgeschlagen (%d)", (int)st);
    g_inited = true;
}

std::string mbed_err(int code) {
    char buf[128];
    mbedtls_strerror(code, buf, sizeof(buf));
    char out[160];
    snprintf(out, sizeof(out), "%s (-0x%04X)", buf, (unsigned)(-code));
    return out;
}

Session::Session() {
    s_ = new mbedtls_ssl_session;
    mbedtls_ssl_session_init(s_);
}
Session::~Session() {
    mbedtls_ssl_session_free(s_);
    delete s_;
}

struct TlsConn::Impl {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    plat::Conn* tcp = nullptr;
    uint32_t cur_timeout = 5000;
    bool bad = false;
};

static int bio_send(void* ctx, const unsigned char* buf, size_t len) {
    TlsConn::Impl* d = (TlsConn::Impl*)ctx;
    if (!d->tcp || !d->tcp->is_open()) return MBEDTLS_ERR_SSL_CONN_EOF;
    if (!d->tcp->write_all(buf, len, d->cur_timeout < 15000 ? 15000 : d->cur_timeout)) return -0x004E; // NET_SEND_FAILED
    return (int)len;
}

static int bio_recv_timeout(void* ctx, unsigned char* buf, size_t len, uint32_t timeout) {
    (void)timeout;
    TlsConn::Impl* d = (TlsConn::Impl*)ctx;
    if (!d->tcp) return MBEDTLS_ERR_SSL_CONN_EOF;
    int n = d->tcp->read(buf, len, d->cur_timeout);
    if (n > 0) return n;
    if (n == 0) return MBEDTLS_ERR_SSL_TIMEOUT;
    return 0; // EOF
}

TlsConn::TlsConn() : d_(new Impl) {
    mbedtls_ssl_init(&d_->ssl);
    mbedtls_ssl_config_init(&d_->conf);
    mbedtls_entropy_init(&d_->entropy);
    mbedtls_ctr_drbg_init(&d_->drbg);
}

TlsConn::~TlsConn() {
    close();
    mbedtls_ssl_free(&d_->ssl);
    mbedtls_ssl_config_free(&d_->conf);
    mbedtls_ctr_drbg_free(&d_->drbg);
    mbedtls_entropy_free(&d_->entropy);
    delete d_;
}

static bool is_ip_literal(const std::string& h) {
    if (h.empty()) return false;
    for (char c : h) if (!((c >= '0' && c <= '9') || c == '.' || c == ':')) return false;
    return true;
}

TlsConn* TlsConn::wrap(plat::Conn* tcp, const Options& opts, std::string* err, Session* reuse) {
    global_init();
    TlsConn* t = new TlsConn();
    t->opts_ = opts;
    Impl* d = t->d_;
    d->tcp = tcp;
    int ret;
    const char* pers = "drucker-dashboard";
    if ((ret = mbedtls_ctr_drbg_seed(&d->drbg, mbedtls_entropy_func, &d->entropy,
                                     (const unsigned char*)pers, strlen(pers))) != 0) {
        if (err) *err = "RNG: " + mbed_err(ret);
        delete t;
        return nullptr;
    }
    if ((ret = mbedtls_ssl_config_defaults(&d->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                           MBEDTLS_SSL_PRESET_DEFAULT)) != 0) {
        if (err) *err = "TLS-Konfiguration: " + mbed_err(ret);
        delete t;
        return nullptr;
    }
    // Wie im Original: Zertifikat NICHT pruefen (Drucker nutzen selbstsignierte Zertifikate)
    mbedtls_ssl_conf_authmode(&d->conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&d->conf, mbedtls_ctr_drbg_random, &d->drbg);
    mbedtls_ssl_conf_min_tls_version(&d->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    if (opts.allow_tls13 && !opts.tls12_only)
        mbedtls_ssl_conf_max_tls_version(&d->conf, MBEDTLS_SSL_VERSION_TLS1_3);
    else
        mbedtls_ssl_conf_max_tls_version(&d->conf, MBEDTLS_SSL_VERSION_TLS1_2);
#if defined(MBEDTLS_SSL_SESSION_TICKETS)
    mbedtls_ssl_conf_session_tickets(&d->conf, MBEDTLS_SSL_SESSION_TICKETS_ENABLED);
#endif
    if ((ret = mbedtls_ssl_setup(&d->ssl, &d->conf)) != 0) {
        if (err) *err = "TLS-Setup (Speicher?): " + mbed_err(ret);
        delete t;
        return nullptr;
    }
    if (!opts.sni.empty() && !is_ip_literal(opts.sni)) mbedtls_ssl_set_hostname(&d->ssl, opts.sni.c_str());
    if (reuse && reuse->valid) {
        if ((ret = mbedtls_ssl_set_session(&d->ssl, reuse->get())) != 0)
            logf("[TLS] Sitzung konnte nicht gesetzt werden: %s", mbed_err(ret).c_str());
    }
    mbedtls_ssl_set_bio(&d->ssl, d, bio_send, nullptr, bio_recv_timeout);

    uint32_t start = plat::millis();
    d->cur_timeout = 2000;
    for (;;) {
        ret = mbedtls_ssl_handshake(&d->ssl);
        if (ret == 0) break;
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE || ret == MBEDTLS_ERR_SSL_TIMEOUT) {
            if (plat::millis() - start > opts.handshake_timeout_ms) {
                if (err) *err = "TLS-Handshake: Zeitueberschreitung";
                delete t;
                return nullptr;
            }
            if (!tcp->is_open()) {
                if (err) *err = "TLS-Handshake: Verbindung vom Gegenueber geschlossen";
                delete t;
                return nullptr;
            }
            continue;
        }
        if (err) *err = "TLS-Handshake: " + mbed_err(ret);
        delete t;
        return nullptr;
    }
    t->open_ = true;
    return t;
}

TlsConn* TlsConn::connect(const std::string& host, uint16_t port, uint32_t timeout_ms, const Options& opts,
                          std::string* err, Session* reuse) {
    plat::Conn* tcp = plat::tcp_connect(host, port, timeout_ms, err);
    if (!tcp) return nullptr;
    Options o = opts;
    if (o.sni.empty()) o.sni = host;
    TlsConn* t = wrap(tcp, o, err, reuse);
    return t;  // wrap() gibt tcp bei Fehler ueber delete t frei
}

int TlsConn::available() {
    if (!pushback_.empty()) return (int)pushback_.size();
    if (!open_) return 0;
    return (int)mbedtls_ssl_get_bytes_avail(&d_->ssl);
}

int TlsConn::read_raw(uint8_t* buf, size_t len, uint32_t timeout_ms) {
    if (!open_) return -1;
    uint32_t start = plat::millis();
    for (;;) {
        uint32_t el = plat::millis() - start;
        if (el >= timeout_ms && timeout_ms > 0) return 0;
        d_->cur_timeout = timeout_ms > el ? timeout_ms - el : 1;
        int ret = mbedtls_ssl_read(&d_->ssl, buf, len);
        if (ret > 0) return ret;
        if (ret == 0) { open_ = false; return -1; }
        if (ret == MBEDTLS_ERR_SSL_TIMEOUT || ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (!d_->tcp->is_open()) { open_ = false; return -1; }
            if (timeout_ms == 0) return 0;
            continue;
        }
#if defined(MBEDTLS_SSL_PROTO_TLS1_3)
        if (ret == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) continue;
#endif
        if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) { open_ = false; return -1; }
        open_ = false;
        return -1;
    }
}

bool TlsConn::write_all(const uint8_t* buf, size_t len, uint32_t timeout_ms) {
    if (!open_) return false;
    size_t off = 0;
    uint32_t start = plat::millis();
    d_->cur_timeout = timeout_ms;
    while (off < len) {
        int ret = mbedtls_ssl_write(&d_->ssl, buf + off, len - off);
        if (ret > 0) { off += (size_t)ret; continue; }
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (plat::millis() - start > timeout_ms) return false;
            continue;
        }
        open_ = false;
        return false;
    }
    return true;
}

void TlsConn::close() {
    if (open_ && opts_.send_close_notify) {
        d_->cur_timeout = 1000;
        mbedtls_ssl_close_notify(&d_->ssl);
    }
    open_ = false;
    if (d_->tcp) {
        d_->tcp->close();
        delete d_->tcp;
        d_->tcp = nullptr;
    }
}

bool TlsConn::is_open() { return open_ && d_->tcp && d_->tcp->is_open(); }

std::string TlsConn::peer() { return d_->tcp ? d_->tcp->peer() : ""; }

bool TlsConn::save_session(Session& out) {
    if (!open_) return false;
    mbedtls_ssl_session_free(out.get());
    mbedtls_ssl_session_init(out.get());
    int ret = mbedtls_ssl_get_session(&d_->ssl, out.get());
    out.valid = (ret == 0);
    if (ret != 0) logf("[TLS] Sitzung konnte nicht gesichert werden: %s", mbed_err(ret).c_str());
    return out.valid;
}

std::string TlsConn::version() { return open_ ? mbedtls_ssl_get_version(&d_->ssl) : ""; }
std::string TlsConn::cipher() { return open_ ? mbedtls_ssl_get_ciphersuite(&d_->ssl) : ""; }

plat::Conn* connect_any(const std::string& host, uint16_t port, bool use_tls, uint32_t timeout_ms,
                        std::string* err, const Options* opts) {
    if (!use_tls) return plat::tcp_connect(host, port, timeout_ms, err);
    Options o;
    if (opts) o = *opts;
    return TlsConn::connect(host, port, timeout_ms, o, err);
}

} // namespace tls
