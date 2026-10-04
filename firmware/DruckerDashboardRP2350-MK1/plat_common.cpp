// Plattformunabhaengige Teile von plat.h (Conn-Hilfsfunktionen, Log-Ringpuffer)
#include "plat.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace plat {

int Conn::read(uint8_t* buf, size_t len, uint32_t timeout_ms) {
    if (!pushback_.empty()) {
        size_t n = pushback_.size() < len ? pushback_.size() : len;
        memcpy(buf, pushback_.data(), n);
        pushback_.erase(0, n);
        return (int)n;
    }
    return read_raw(buf, len, timeout_ms);
}

bool Conn::read_exact(uint8_t* buf, size_t len, uint32_t timeout_ms) {
    size_t got = 0;
    uint32_t start = millis();
    while (got < len) {
        uint32_t el = millis() - start;
        if (el >= timeout_ms) return false;
        int n = read(buf + got, len - got, timeout_ms - el);
        if (n < 0) return false;
        got += (size_t)n;
    }
    return true;
}

int Conn::read_line(std::string& out, size_t maxlen, uint32_t timeout_ms) {
    out.clear();
    uint32_t start = millis();
    uint8_t tmp[256];
    for (;;) {
        // Zuerst im Pushback-Puffer nach Zeilenende suchen
        size_t nl = pushback_.find('\n');
        if (nl != std::string::npos) {
            out.append(pushback_, 0, nl);
            pushback_.erase(0, nl + 1);
            if (!out.empty() && out.back() == '\r') out.pop_back();
            return 1;
        }
        out.append(pushback_);
        pushback_.clear();
        if (out.size() > maxlen) return -1;
        uint32_t el = millis() - start;
        if (el >= timeout_ms) {
            // Teilzeile zurueck in den Puffer
            pushback_ = out;
            out.clear();
            return 0;
        }
        int n = read_raw(tmp, sizeof(tmp), timeout_ms - el);
        if (n < 0) return -1;
        if (n > 0) pushback_.append((const char*)tmp, (size_t)n);
    }
}

} // namespace plat

// ---------------------------------------------------------------------
// Log-Ringpuffer: die letzten ~12 KB Logzeilen, im Browser unter
// Einstellungen -> "Diagnose-Log" einsehbar (Ersatz fuer die Server-
// Konsole des Python-Originals, z. B. "[MK6-MQTT]"-Zeilen).
// ---------------------------------------------------------------------
static plat::Mutex* g_log_mutex = nullptr;
static char g_log_buf[12288];
static size_t g_log_len = 0;

extern void plat_log_out(const char* line); // Serial / stdout

void logf(const char* fmt, ...) {
    char line[384];
    std::string ts = plat::time_hms();
    int off = snprintf(line, sizeof(line), "%s ", ts.c_str());
    if (off < 0) off = 0;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + off, sizeof(line) - (size_t)off, fmt, ap);
    va_end(ap);
    size_t l = strlen(line);
    if (l + 2 < sizeof(line)) { line[l++] = '\n'; line[l] = 0; }
    plat_log_out(line);
    if (!g_log_mutex) g_log_mutex = new plat::Mutex();
    plat::Lock lk(*g_log_mutex);
    if (l >= sizeof(g_log_buf)) return;
    if (g_log_len + l > sizeof(g_log_buf)) {
        // vorne so viel verwerfen, dass die neue Zeile passt (an Zeilengrenze)
        size_t drop = g_log_len + l - sizeof(g_log_buf);
        while (drop < g_log_len && g_log_buf[drop] != '\n') drop++;
        if (drop < g_log_len) drop++;
        memmove(g_log_buf, g_log_buf + drop, g_log_len - drop);
        g_log_len -= drop;
    }
    memcpy(g_log_buf + g_log_len, line, l);
    g_log_len += l;
}

std::string log_dump() {
    if (!g_log_mutex) g_log_mutex = new plat::Mutex();
    plat::Lock lk(*g_log_mutex);
    return std::string(g_log_buf, g_log_len);
}
