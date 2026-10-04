// Kleine Hilfsfunktionen (Strings, URLs, Base64, MD5, IDs)
#pragma once
#include <stdint.h>
#include <string>
#include <vector>

namespace util {

std::string trim(const std::string& s);
std::string lower(std::string s);
std::string upper(std::string s);
bool starts_with(const std::string& s, const std::string& p);
bool ends_with(const std::string& s, const std::string& p);
bool iequals(const std::string& a, const std::string& b);
std::vector<std::string> split(const std::string& s, char sep);
std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));

// 10-stellige Hex-ID wie uuid.uuid4().hex[:10] im Original
std::string new_id(size_t len = 10);

std::string url_decode(const std::string& s);
std::string url_encode(const std::string& s);   // RFC 3986 (unreserved bleibt)
// Liefert Wert eines Query-Parameters ("" falls fehlt)
std::string query_param(const std::string& query, const std::string& key, bool* found = nullptr);

struct Url {
    std::string scheme, user, pass, host, path; // path inkl. Query, beginnt mit '/'
    uint16_t port = 0;
    bool ok = false;
};
Url parse_url(const std::string& url);

std::string base64_encode(const uint8_t* data, size_t len);
std::string base64_encode(const std::string& s);
std::string base64_decode(const std::string& s);
std::string md5_hex(const std::string& s);

bool is_ipv4(const std::string& s);

// JSON-String-Escaping (fuer handgebaute JSON-Antworten)
std::string json_escape(const std::string& s);

} // namespace util
