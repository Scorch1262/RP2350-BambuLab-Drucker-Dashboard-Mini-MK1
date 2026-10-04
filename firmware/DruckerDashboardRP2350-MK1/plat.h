// =====================================================================
// Plattform-Abstraktion
//
// Die gesamte Dashboard-Logik (Druckeranbindungen, HTTP-Server, API,
// Kamera-Relays) ist plattformneutral geschrieben und benutzt nur die
// hier deklarierten Funktionen/Klassen. Es gibt zwei Implementierungen:
//
//   plat_arduino.cpp  - RP2350 (arduino-pico, FreeRTOS SMP, lwIP/W5500,
//                        LittleFS)
//   ../../host/plat_posix.cpp - Linux (POSIX-Sockets, std::thread) -
//                        NUR zum Testen der Logik auf einem PC gegen
//                        simulierte Drucker, nicht Teil der Firmware.
// =====================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string>

namespace plat {

// ---- Zeit ----------------------------------------------------------
uint32_t millis();
void sleep_ms(uint32_t ms);
// Unix-Zeit in Sekunden, 0 solange keine Uhrzeit bekannt ist (NTP).
int64_t epoch();
// "HH:MM:SS" in lokaler Zeit; falls (noch) keine Uhrzeit bekannt ist,
// die Laufzeit seit dem Start im Format "+HH:MM:SS".
std::string time_hms();
// Zufallszahl (Hardware-RNG auf dem RP2350)
uint32_t random32();

// ---- Nebenlaeufigkeit ---------------------------------------------
class Mutex {
public:
    Mutex();
    ~Mutex();
    void lock();
    void unlock();
private:
    void* h_;
    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;
};

class Lock {
public:
    explicit Lock(Mutex& m) : m_(m) { m_.lock(); }
    ~Lock() { m_.unlock(); }
private:
    Mutex& m_;
};

// Startet eine eigenstaendige Aufgabe (FreeRTOS-Task bzw. std::thread).
// stack_bytes wird nur auf dem Geraet beachtet.
bool task_start(const char* name, void (*fn)(void*), void* arg, uint32_t stack_bytes, int prio = 2);
// Freier Heap in Bytes (Host: 0)
uint32_t free_heap();
uint32_t total_heap();
// true, wenn (auf dem Geraet) mindestens need Bytes Heap frei sind
inline bool heap_ok(uint32_t need) { uint32_t f = free_heap(); return f == 0 || f >= need; }

// ---- Netzwerk ------------------------------------------------------
// Gemeinsame Schnittstelle fuer reine TCP- und TLS-Verbindungen.
class Conn {
public:
    virtual ~Conn() {}
    // >0: Anzahl gelesener Bytes, 0: Timeout ohne Daten, <0: Verbindung
    // geschlossen/Fehler. Liefert zuerst eventuell von read_line()
    // vorgelesene Bytes.
    int read(uint8_t* buf, size_t len, uint32_t timeout_ms);
    // Rohes Lesen (von TCP/TLS implementiert)
    virtual int read_raw(uint8_t* buf, size_t len, uint32_t timeout_ms) = 0;
    // Anzahl sofort lesbarer Bytes (>0), ohne zu blockieren (0 = nichts/unbekannt)
    virtual int available() { return (int)pushback_.size(); }
    // Schreibt alles oder liefert false.
    virtual bool write_all(const uint8_t* buf, size_t len, uint32_t timeout_ms = 15000) = 0;
    virtual void close() = 0;
    virtual bool is_open() = 0;
    // Gegenstelle (nur fuer Logausgaben)
    virtual std::string peer() { return ""; }

    bool write_str(const std::string& s, uint32_t timeout_ms = 15000) {
        return write_all((const uint8_t*)s.data(), s.size(), timeout_ms);
    }
    // Liest genau len Bytes (true) oder scheitert (false).
    bool read_exact(uint8_t* buf, size_t len, uint32_t timeout_ms);
    // Liest eine Zeile bis '\n' (ohne CR/LF). 1 = ok, 0 = Timeout, -1 = Fehler/zu lang
    int read_line(std::string& out, size_t maxlen, uint32_t timeout_ms);
    // Bytes zurueck in den Lesepuffer legen (werden von read() zuerst geliefert)
    void unread(const uint8_t* buf, size_t len) { pushback_.insert(0, (const char*)buf, len); }
protected:
    std::string pushback_;
};

// Reine TCP-Verbindung aufbauen. Liefert nullptr bei Fehler (err gesetzt).
Conn* tcp_connect(const std::string& host, uint16_t port, uint32_t timeout_ms, std::string* err);

class TcpServer {
public:
    TcpServer();
    ~TcpServer();
    bool begin(uint16_t port);
    // Nicht-blockierend: neue Verbindung oder nullptr.
    Conn* accept();
private:
    void* impl_;
};

// Eigene IP-Adresse als Text ("0.0.0.0" falls unbekannt)
std::string local_ip();
std::string mac_address();
bool link_up();

// ---- Dateisystem (nur fuer config.json) ---------------------------
bool fs_read(const char* path, std::string& out);
bool fs_write(const char* path, const std::string& data);
bool fs_remove(const char* path);

// ---- System --------------------------------------------------------
void reboot();
const char* platform_name();

// ---- Firmware-Update (nur Geraet) ---------------------------------
bool ota_begin(size_t size, std::string* err);
bool ota_write(const uint8_t* data, size_t len);
bool ota_end(std::string* err);
void ota_abort();

} // namespace plat

// ---- Logging (Ringpuffer, ueber /api/log im Browser einsehbar) ----
void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
std::string log_dump();
