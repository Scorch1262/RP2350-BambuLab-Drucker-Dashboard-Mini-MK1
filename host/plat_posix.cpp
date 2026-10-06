// =====================================================================
// Linux-Implementierung von plat.h - NUR fuer Tests auf einem PC.
// Erlaubt, die komplette Dashboard-Logik (gleicher Quellcode wie die
// Firmware) gegen simulierte Drucker laufen zu lassen.
// =====================================================================
#include "plat.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <chrono>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

std::string g_host_data_dir = "./data";
uint16_t g_host_port_override = 0;

void plat_log_out(const char* line) {
    fputs(line, stdout);
    fflush(stdout);
}

namespace plat {

static auto g_start = std::chrono::steady_clock::now();

uint32_t millis() {
    return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - g_start).count();
}
void sleep_ms(uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
int64_t epoch() { return (int64_t)time(nullptr); }
std::string time_hms() {
    time_t t = time(nullptr);
    struct tm lt;
    localtime_r(&t, &lt);
    char b[16];
    strftime(b, sizeof(b), "%H:%M:%S", &lt);
    return b;
}
uint32_t random32() {
    static FILE* f = fopen("/dev/urandom", "rb");
    uint32_t v = 0;
    if (f) fread(&v, 1, 4, f);
    return v;
}

Mutex::Mutex() : h_(new std::recursive_mutex()) {}
Mutex::~Mutex() { delete (std::recursive_mutex*)h_; }
void Mutex::lock() { ((std::recursive_mutex*)h_)->lock(); }
void Mutex::unlock() { ((std::recursive_mutex*)h_)->unlock(); }

bool task_start(const char* name, void (*fn)(void*), void* arg, uint32_t stack_bytes, int prio) {
    (void)name; (void)stack_bytes; (void)prio;
    std::thread(fn, arg).detach();
    return true;
}
uint32_t free_heap() { return 0; }
uint32_t total_heap() { return 0; }
std::string boot_reason() { return "Host-Test"; }
std::string sched_text() { return ""; }
bool safe_mode() { return getenv("DD_SAFE_MODE") != nullptr; }
void mark_stable() {}

// ---- TCP --------------------------------------------------------------
class PosixConn : public Conn {
public:
    explicit PosixConn(int fd, const std::string& peer) : fd_(fd), peer_(peer) {}
    ~PosixConn() override { close(); }
    int read_raw(uint8_t* buf, size_t len, uint32_t timeout_ms) override {
        if (fd_ < 0) return -1;
        struct pollfd p = {fd_, POLLIN, 0};
        int r = ::poll(&p, 1, (int)timeout_ms);
        if (r == 0) return 0;
        if (r < 0) return errno == EINTR ? 0 : -1;
        ssize_t n = ::recv(fd_, buf, len, 0);
        if (n > 0) return (int)n;
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
        open_ = false;
        return -1;
    }
    int available() override {
        if (!pushback_.empty()) return (int)pushback_.size();
        int n = 0;
        if (fd_ >= 0) {
            struct pollfd p = {fd_, POLLIN, 0};
            if (::poll(&p, 1, 0) > 0) n = 1;
        }
        return n;
    }
    bool write_all(const uint8_t* buf, size_t len, uint32_t timeout_ms) override {
        size_t off = 0;
        uint32_t start = millis();
        while (off < len) {
            if (fd_ < 0) return false;
            struct pollfd p = {fd_, POLLOUT, 0};
            int r = ::poll(&p, 1, 200);
            if (r <= 0) {
                if (millis() - start > timeout_ms) return false;
                continue;
            }
            ssize_t n = ::send(fd_, buf + off, len - off, MSG_NOSIGNAL);
            if (n <= 0) {
                if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
                open_ = false;
                return false;
            }
            off += (size_t)n;
        }
        return true;
    }
    void close() override {
        if (fd_ >= 0) {
            ::shutdown(fd_, SHUT_RDWR);
            ::close(fd_);
        }
        fd_ = -1;
        open_ = false;
    }
    bool is_open() override { return fd_ >= 0 && open_; }
    std::string peer() override { return peer_; }

private:
    int fd_;
    bool open_ = true;
    std::string peer_;
};

Conn* tcp_connect(const std::string& host, uint16_t port, uint32_t timeout_ms, std::string* err) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    char ps[8];
    snprintf(ps, sizeof(ps), "%u", port);
    if (getaddrinfo(host.c_str(), ps, &hints, &res) != 0 || !res) {
        if (err) *err = "Hostname nicht aufloesbar: " + host;
        return nullptr;
    }
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    fcntl(fd, F_SETFL, O_NONBLOCK);
    int r = ::connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (r < 0 && errno != EINPROGRESS) {
        if (err) *err = std::string("Verbindung fehlgeschlagen: ") + strerror(errno);
        ::close(fd);
        return nullptr;
    }
    struct pollfd p = {fd, POLLOUT, 0};
    r = ::poll(&p, 1, (int)timeout_ms);
    int soerr = 0;
    socklen_t sl = sizeof(soerr);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
    if (r <= 0 || soerr != 0) {
        if (err) *err = r <= 0 ? "Zeitueberschreitung beim Verbindungsaufbau" : std::string("Verbindung fehlgeschlagen: ") + strerror(soerr);
        ::close(fd);
        return nullptr;
    }
    int fl = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return new PosixConn(fd, host + ":" + ps);
}

struct ServerImpl { int fd = -1; };

TcpServer::TcpServer() : impl_(new ServerImpl) {}
TcpServer::~TcpServer() {
    ServerImpl* s = (ServerImpl*)impl_;
    if (s->fd >= 0) ::close(s->fd);
    delete s;
}
bool TcpServer::begin(uint16_t port) {
    signal(SIGPIPE, SIG_IGN);
    ServerImpl* s = (ServerImpl*)impl_;
    if (g_host_port_override) port = g_host_port_override;
    s->fd = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(s->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = INADDR_ANY;
    if (::bind(s->fd, (struct sockaddr*)&a, sizeof(a)) < 0) {
        perror("bind");
        return false;
    }
    ::listen(s->fd, 16);
    fcntl(s->fd, F_SETFL, O_NONBLOCK);
    return true;
}
Conn* TcpServer::accept() {
    ServerImpl* s = (ServerImpl*)impl_;
    struct sockaddr_in a;
    socklen_t al = sizeof(a);
    int fd = ::accept(s->fd, (struct sockaddr*)&a, &al);
    if (fd < 0) return nullptr;
    int fl = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
    char ip[32];
    inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
    return new PosixConn(fd, ip);
}

std::string local_ip() { return "127.0.0.1"; }
std::string mac_address() { return "02:00:00:00:00:01"; }
bool link_up() { return true; }

// ---- Dateien ----------------------------------------------------------
static std::string path_of(const char* p) { return g_host_data_dir + (p[0] == '/' ? "" : "/") + p; }
bool fs_read(const char* path, std::string& out) {
    std::ifstream f(path_of(path), std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}
bool fs_write(const char* path, const std::string& data) {
    mkdir(g_host_data_dir.c_str(), 0755);
    std::string tmp = path_of(path) + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << data;
    }
    return rename(tmp.c_str(), path_of(path).c_str()) == 0;
}
bool fs_remove(const char* path) { return ::remove(path_of(path).c_str()) == 0; }

void reboot() {
    logf("[HOST] Neustart angefordert - Host-Build beendet sich.");
    exit(0);
}
const char* platform_name() { return "Linux-Host (Test)"; }

bool ota_begin(size_t, std::string* err) { if (err) *err = "Firmware-Update ist im Host-Testbetrieb nicht verfuegbar."; return false; }
bool ota_write(const uint8_t*, size_t) { return false; }
bool ota_end(std::string* err) { if (err) *err = "nicht verfuegbar"; return false; }
void ota_abort() {}

} // namespace plat
