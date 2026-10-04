#include "util.h"
#include "plat.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mbedtls/md5.h"
#include "mbedtls/base64.h"

namespace util {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}
std::string lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}
std::string upper(std::string s) {
    for (auto& c : s) c = (char)toupper((unsigned char)c);
    return s;
}
bool starts_with(const std::string& s, const std::string& p) { return s.size() >= p.size() && s.compare(0, p.size(), p) == 0; }
bool ends_with(const std::string& s, const std::string& p) { return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0; }
bool iequals(const std::string& a, const std::string& b) { return lower(a) == lower(b); }

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        size_t p = s.find(sep, start);
        if (p == std::string::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, p - start));
        start = p + 1;
    }
    return out;
}

std::string fmt(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    int n = vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    if (n < (int)sizeof(buf)) return std::string(buf, n < 0 ? 0 : (size_t)n);
    std::string out((size_t)n + 1, '\0');
    va_start(ap, f);
    vsnprintf(&out[0], out.size(), f, ap);
    va_end(ap);
    out.resize((size_t)n);
    return out;
}

std::string new_id(size_t len) {
    static const char* hx = "0123456789abcdef";
    std::string s;
    while (s.size() < len) {
        uint32_t r = plat::random32();
        for (int i = 0; i < 8 && s.size() < len; i++) {
            s.push_back(hx[r & 0xF]);
            r >>= 4;
        }
    }
    return s;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string url_decode(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && hexval(s[i + 1]) >= 0 && hexval(s[i + 2]) >= 0) {
            o.push_back((char)(hexval(s[i + 1]) * 16 + hexval(s[i + 2])));
            i += 2;
        } else if (s[i] == '+') {
            o.push_back(' ');
        } else {
            o.push_back(s[i]);
        }
    }
    return o;
}

std::string url_encode(const std::string& s) {
    static const char* hx = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o.push_back((char)c);
        else {
            o.push_back('%');
            o.push_back(hx[c >> 4]);
            o.push_back(hx[c & 15]);
        }
    }
    return o;
}

std::string query_param(const std::string& query, const std::string& key, bool* found) {
    if (found) *found = false;
    for (auto& part : split(query, '&')) {
        size_t eq = part.find('=');
        std::string k = url_decode(eq == std::string::npos ? part : part.substr(0, eq));
        if (k == key) {
            if (found) *found = true;
            return eq == std::string::npos ? "" : url_decode(part.substr(eq + 1));
        }
    }
    return "";
}

Url parse_url(const std::string& url) {
    Url u;
    size_t p = url.find("://");
    if (p == std::string::npos) return u;
    u.scheme = lower(url.substr(0, p));
    std::string rest = url.substr(p + 3);
    size_t slash = rest.find('/');
    std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
    u.path = slash == std::string::npos ? "/" : rest.substr(slash);
    size_t at = authority.rfind('@');
    if (at != std::string::npos) {
        std::string cred = authority.substr(0, at);
        authority = authority.substr(at + 1);
        size_t c = cred.find(':');
        u.user = url_decode(c == std::string::npos ? cred : cred.substr(0, c));
        u.pass = c == std::string::npos ? "" : url_decode(cred.substr(c + 1));
    }
    if (!authority.empty() && authority[0] == '[') {
        size_t e = authority.find(']');
        if (e == std::string::npos) return u;
        u.host = authority.substr(1, e - 1);
        if (e + 1 < authority.size() && authority[e + 1] == ':') u.port = (uint16_t)atoi(authority.c_str() + e + 2);
    } else {
        size_t c = authority.rfind(':');
        if (c != std::string::npos) {
            u.host = authority.substr(0, c);
            u.port = (uint16_t)atoi(authority.c_str() + c + 1);
        } else {
            u.host = authority;
        }
    }
    if (u.port == 0) {
        if (u.scheme == "http") u.port = 80;
        else if (u.scheme == "https") u.port = 443;
        else if (u.scheme == "rtsp") u.port = 554;
        else if (u.scheme == "rtsps") u.port = 322;
    }
    u.ok = !u.host.empty();
    return u;
}

std::string base64_encode(const uint8_t* data, size_t len) {
    size_t olen = 0;
    mbedtls_base64_encode(nullptr, 0, &olen, data, len);
    std::string o(olen, '\0');
    if (mbedtls_base64_encode((unsigned char*)&o[0], o.size(), &olen, data, len) != 0) return "";
    o.resize(olen);
    return o;
}
std::string base64_encode(const std::string& s) { return base64_encode((const uint8_t*)s.data(), s.size()); }
std::string base64_decode(const std::string& s) {
    size_t olen = 0;
    mbedtls_base64_decode(nullptr, 0, &olen, (const unsigned char*)s.data(), s.size());
    std::string o(olen, '\0');
    if (olen == 0) return "";
    if (mbedtls_base64_decode((unsigned char*)&o[0], o.size(), &olen, (const unsigned char*)s.data(), s.size()) != 0) return "";
    o.resize(olen);
    return o;
}

std::string md5_hex(const std::string& s) {
    unsigned char out[16];
    mbedtls_md5((const unsigned char*)s.data(), s.size(), out);
    static const char* hx = "0123456789abcdef";
    std::string o;
    for (int i = 0; i < 16; i++) {
        o.push_back(hx[out[i] >> 4]);
        o.push_back(hx[out[i] & 15]);
    }
    return o;
}

bool is_ipv4(const std::string& s) {
    int parts = 0;
    size_t i = 0;
    while (i < s.size()) {
        size_t j = i;
        int v = 0;
        while (j < s.size() && isdigit((unsigned char)s[j])) {
            v = v * 10 + (s[j] - '0');
            if (v > 255) return false;
            j++;
        }
        if (j == i || j - i > 3) return false;
        parts++;
        if (j == s.size()) break;
        if (s[j] != '.') return false;
        i = j + 1;
        if (i == s.size()) return false;
    }
    return parts == 4;
}

std::string json_escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 2);
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    snprintf(b, sizeof(b), "\\u%04x", c);
                    o += b;
                } else {
                    o.push_back((char)c);
                }
        }
    }
    return o;
}

} // namespace util
