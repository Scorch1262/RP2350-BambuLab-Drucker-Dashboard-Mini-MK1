// =====================================================================
// RP2350-Implementierung von plat.h (arduino-pico ohne FreeRTOS,
// kooperative Aufgaben aus coop.cpp, lwIP
// ueber W5500 im MACRAW-Modus, LittleFS)
// =====================================================================
#ifdef ARDUINO
#include <hardware/watchdog.h>
#include <Arduino.h>
#include <LittleFS.h>
#include <WiFiClient.h>
#include <WiFiServer.h>
#include <LwipEthernet.h>
#include <Updater.h>
#include <time.h>
#include "plat.h"
#include "coop.h"

// in der .ino-Datei definiert (Zugriff auf das Ethernet-Objekt)
std::string dev_local_ip();
std::string dev_mac();
bool dev_link_up();

void plat_log_out(const char* line) {
    Serial.print(line);
}

namespace plat {

uint32_t millis() { return ::millis(); }
void sleep_ms(uint32_t ms) { ::delay(ms); }

int64_t epoch() {
    time_t t = time(nullptr);
    return t > 1700000000 ? (int64_t)t : 0;
}

std::string time_hms() {
    char b[24];
    time_t t = time(nullptr);
    if (t > 1700000000) {
        struct tm lt;
        localtime_r(&t, &lt);
        strftime(b, sizeof(b), "%H:%M:%S", &lt);
    } else {
        uint32_t s = ::millis() / 1000;
        snprintf(b, sizeof(b), "+%02u:%02u:%02u", (unsigned)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
    }
    return b;
}

uint32_t random32() { return rp2040.hwrand32(); }

// Kooperativer, rekursiver Mutex: wer ihn nicht bekommt, gibt ab.
struct MtxState {
    coop::Task* owner;
    int count;
};
static coop::Task* const kNoTask = (coop::Task*)1;   // setup()/loop()-Kontext
static coop::Task* me() {
    coop::Task* t = coop::current();
    return t ? t : kNoTask;
}
Mutex::Mutex() : h_(new MtxState{nullptr, 0}) {}
Mutex::~Mutex() { delete (MtxState*)h_; }
void Mutex::lock() {
    MtxState* s = (MtxState*)h_;
    coop::Task* m = me();
    while (s->count > 0 && s->owner != m) coop::sleep(0);
    s->owner = m;
    s->count++;
}
void Mutex::unlock() {
    MtxState* s = (MtxState*)h_;
    if (s->count > 0 && --s->count == 0) s->owner = nullptr;
}

bool task_start(const char* name, void (*fn)(void*), void* arg, uint32_t stack_bytes, int prio) {
    (void)prio;
    return coop::start(name, fn, arg, stack_bytes);
}

uint32_t free_heap() { return rp2040.getFreeHeap(); }
uint32_t total_heap() { return rp2040.getTotalHeap(); }

// ---- Absturz-Diagnose (Merker g_dd_crash in coop.cpp) -------------------
struct BootInfo { uint32_t magic; uint32_t crash_boots; uint32_t intentional; };
static BootInfo __uninitialized_ram(g_boot);
#define DD_BOOT_MAGIC 0x44424F4Fu
#define DD_INTENT_MAGIC 0x494E5445u
static bool g_safe = false;

bool safe_mode() {
    boot_reason();
    return g_safe;
}

void mark_stable() {
    if (g_boot.magic == DD_BOOT_MAGIC) g_boot.crash_boots = 0;
}

std::string sched_text() {
    coop::Stats st = coop::stats();
    char b[160];
    snprintf(b, sizeof(b), "%d Aufgaben, laengste Blockade %u ms (%s), %u x ueber 250 ms", coop::count(),
             (unsigned)st.max_block_ms, st.max_block_task, (unsigned)st.long_count);
    return b;
}

std::string boot_reason() {
    static std::string r;
    if (!r.empty()) return r;
    switch (rp2040.getResetReason()) {
        case RP2040::PWRON_RESET: r = "Einschalten"; break;
        case RP2040::RUN_PIN_RESET: r = "Reset-Taster"; break;
        case RP2040::SOFT_RESET: r = "Neustart durch Software"; break;
        case RP2040::WDT_RESET: r = "Watchdog (Programm haengte)"; break;
        case RP2040::DEBUG_RESET: r = "Debugger"; break;
        case RP2040::GLITCH_RESET: r = "Spannungsstoerung"; break;
        case RP2040::BROWNOUT_RESET: r = "Unterspannung"; break;
        default: r = "unbekannt"; break;
    }
    bool crash = rp2040.getResetReason() == RP2040::WDT_RESET || g_dd_crash.magic == DD_CRASH_MAGIC;
    if (g_boot.magic != DD_BOOT_MAGIC) {
        g_boot.magic = DD_BOOT_MAGIC;
        g_boot.crash_boots = 0;
        g_boot.intentional = 0;
    }
    if (g_boot.intentional == DD_INTENT_MAGIC) {
        crash = false;
        r = "Neustart ueber die Weboberflaeche/Update";
    }
    g_boot.intentional = 0;
    g_boot.crash_boots = crash ? g_boot.crash_boots + 1 : 0;
    g_safe = g_boot.crash_boots >= 3;
    if (g_dd_crash.magic == DD_CRASH_MAGIC) {
        g_dd_crash.what[sizeof(g_dd_crash.what) - 1] = 0;
        g_dd_crash.task[sizeof(g_dd_crash.task) - 1] = 0;
        r += std::string(" nach ") + g_dd_crash.what + " in Aufgabe '" + g_dd_crash.task + "'";
    }
    g_dd_crash.magic = 0;
    return r;
}

// ---- TCP ueber lwIP (WiFiClient funktioniert auch mit dem W5500) -------
class DevConn : public Conn {
public:
    explicit DevConn(const WiFiClient& c) : c_(c) {
        c_.setNoDelay(true);
        IPAddress ip = c_.remoteIP();
        peer_ = ip.toString().c_str();
    }
    ~DevConn() override { close(); }
    int read_raw(uint8_t* buf, size_t len, uint32_t timeout_ms) override {
        coop::maybe_yield();
        uint32_t start = ::millis();
        for (;;) {
            int a = c_.available();
            if (a > 0) {
                int n = c_.read(buf, len < (size_t)a ? len : (size_t)a);
                if (n > 0) return n;
            }
            if (!c_.connected()) {
                if (c_.available() > 0) continue;
                open_ = false;
                return -1;
            }
            if (::millis() - start >= timeout_ms) return 0;
            ::delay(1);
        }
    }
    int available() override {
        if (!pushback_.empty()) return (int)pushback_.size();
        return c_.available();
    }
    bool write_all(const uint8_t* buf, size_t len, uint32_t timeout_ms) override {
        size_t off = 0;
        uint32_t start = ::millis();
        c_.setTimeout(timeout_ms);
        while (off < len) {
            coop::maybe_yield();
            if (!c_.connected()) {
                open_ = false;
                return false;
            }
            size_t n = c_.write(buf + off, len - off);
            if (n == 0) {
                if (::millis() - start > timeout_ms) return false;
                ::delay(2);
                continue;
            }
            off += n;
        }
        return true;
    }
    void close() override {
        if (open_) {
            // WiFiClient::stop() wartet bis zu 300 ms OHNE abzugeben auf die
            // Bestaetigung (ACK) der Gegenseite - das wuerde alle Aufgaben
            // anhalten. Daher selbst warten (mit Abgabe), dann sofort schliessen.
            uint32_t start = ::millis();
            while (c_.connected() && !c_.flush(1) && ::millis() - start < 2000) ::delay(2);
            c_.stop(1);
        }
        open_ = false;
    }
    bool is_open() override { return open_ && (c_.connected() || c_.available() > 0); }
    std::string peer() override { return peer_; }

private:
    WiFiClient c_;
    bool open_ = true;
    std::string peer_;
};

Conn* tcp_connect(const std::string& host, uint16_t port, uint32_t timeout_ms, std::string* err) {
    WiFiClient c;
    c.setTimeout(timeout_ms);
    // arduino-pico kann nur eine DNS-Abfrage gleichzeitig -> serialisieren
    bool ok;
    IPAddress ip;
    if (ip.fromString(host.c_str())) {
        ok = c.connect(ip, port);
    } else {
        static Mutex* dns = new Mutex();
        bool found;
        {
            Lock lk(*dns);
            found = ::hostByName(host.c_str(), ip, (int)timeout_ms) == 1;
        }
        ok = found && c.connect(ip, port);
    }
    if (!ok) {
        if (err) *err = "Verbindung zu " + host + ":" + std::to_string(port) + " fehlgeschlagen (nicht erreichbar oder Port geschlossen)";
        return nullptr;
    }
    return new DevConn(c);
}

TcpServer::TcpServer() : impl_(nullptr) {}
TcpServer::~TcpServer() { delete (WiFiServer*)impl_; }
bool TcpServer::begin(uint16_t port) {
    WiFiServer* s = new WiFiServer(port);
    s->begin();
    s->setNoDelay(true);
    impl_ = s;
    return true;
}
Conn* TcpServer::accept() {
    WiFiServer* s = (WiFiServer*)impl_;
    WiFiClient c = s->accept();
    if (!c) return nullptr;
    return new DevConn(c);
}

std::string local_ip() { return dev_local_ip(); }
std::string mac_address() { return dev_mac(); }
bool link_up() { return dev_link_up(); }

// ---- LittleFS (nur serialisiert benutzen) ------------------------------
static Mutex* g_fs = nullptr;
static Mutex& fsm() {
    if (!g_fs) g_fs = new Mutex();
    return *g_fs;
}

bool fs_read(const char* path, std::string& out) {
    Lock lk(fsm());
    File f = LittleFS.open(path, "r");
    if (!f) return false;
    out.clear();
    out.reserve(f.size());
    uint8_t buf[256];
    while (f.available()) {
        int n = f.read(buf, sizeof(buf));
        if (n <= 0) break;
        out.append((const char*)buf, (size_t)n);
    }
    f.close();
    return true;
}

bool fs_write(const char* path, const std::string& data) {
    Lock lk(fsm());
    std::string tmp = std::string(path) + ".tmp";
    File f = LittleFS.open(tmp.c_str(), "w");
    if (!f) return false;
    size_t w = f.write((const uint8_t*)data.data(), data.size());
    f.close();
    if (w != data.size()) return false;
    LittleFS.remove(path);
    return LittleFS.rename(tmp.c_str(), path);
}

bool fs_remove(const char* path) {
    Lock lk(fsm());
    return LittleFS.remove(path);
}

void reboot() {
    g_boot.magic = DD_BOOT_MAGIC;
    g_boot.intentional = DD_INTENT_MAGIC;
    ::delay(200);
    rp2040.reboot();
}

const char* platform_name() { return "RP2350 (Seengreat RP2350-Mini-ETH, W5500)"; }

// ---- Firmware-Update (.bin) ueber LittleFS-Zwischenspeicher -------------
bool ota_begin(size_t size, std::string* err) {
    Lock lk(fsm());
    if (!Update.begin(size)) {
        if (err) *err = "Firmware passt nicht in den Zwischenspeicher (zu gross?) - Update ueber BOOTSEL/UF2 durchfuehren.";
        return false;
    }
    return true;
}
bool ota_write(const uint8_t* data, size_t len) {
    Lock lk(fsm());
    return Update.write((uint8_t*)data, len) == len;
}
bool ota_end(std::string* err) {
    Lock lk(fsm());
    if (!Update.end(true)) {
        if (err) *err = std::string("Firmware-Update fehlgeschlagen (Fehler ") + std::to_string(Update.getError()) + ").";
        return false;
    }
    return true;
}
void ota_abort() {
    Lock lk(fsm());
    Update.end(false);
}

} // namespace plat
#endif
