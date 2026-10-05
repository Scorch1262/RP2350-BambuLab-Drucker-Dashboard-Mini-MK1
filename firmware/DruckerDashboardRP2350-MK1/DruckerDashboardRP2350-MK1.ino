// =====================================================================
// Drucker Dashboard RP2350 (MK1) - Firmware fuer das
// Seengreat RP2350-Mini-ETH (RP2350A + W5500)
//
// Portierung des Python/Flask-Projekts "Drucker Dashboard MK6 v2.5.5"
// auf den Mikrocontroller. Ohne Druckauftrags-Verlauf und Warteschlange.
//
// Pinbelegung laut Seengreat-Schaltplan (RP2350-Eth V1.1):
//   W5500: MISO GP16, CS GP17, SCK GP18, MOSI GP19, RST GP20, INT GP21
//   WS2812-Status-LED: GP25
//
// Bauen: siehe README.md (arduino-pico >= 6.2, Board "Raspberry Pi Pico 2",
// Operating System "FreeRTOS SMP", Flash "4MB (Sketch: 3MB, FS: 1MB)",
// IP-Stack "IPv4 Only - 32K" bzw. tools/build.sh).
// =====================================================================
#include <FreeRTOS.h>
#include <task.h>
#include <W5500lwIP.h>
#include <LittleFS.h>
#include <SimpleMDNS.h>
#include <LwipEthernet.h>
#include <WiFiNTP.h>
#include <time.h>
#include <mbedtls_dd.h>
#include <ArduinoJson.h>
#include <string>
#include "plat.h"
#include "config.h"
#include "app_main.h"
#include "status_led.h"

#define PIN_ETH_MISO 16
#define PIN_ETH_CS 17
#define PIN_ETH_SCK 18
#define PIN_ETH_MOSI 19
#define PIN_ETH_RST 20
#define PIN_ETH_INT 21
#define DD_PIN_LED 25

// Der W5500 wird abgefragt (Polling) statt ueber die INT-Leitung - das
// funktioniert unabhaengig davon, ob/wie INT auf der Platine verdrahtet ist.
Wiznet5500lwIP eth(PIN_ETH_CS, SPI, -1);

extern char wifi_station_hostname[];

std::string dev_local_ip() { return eth.localIP().toString().c_str(); }
bool dev_link_up() { return eth.connected(); }
std::string dev_mac() {
    uint8_t m[6];
    eth.macAddress(m);
    char b[24];
    snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
    return b;
}

static bool g_started = false;
static bool g_mdns = false;
static std::string g_hostname = "drucker-dashboard";

static IPAddress ip_of(const char* s) {
    IPAddress a;
    if (!s || !*s || !a.fromString(s)) return IPAddress(0, 0, 0, 0);
    return a;
}

// ---------------------------------------------------------------------
// WICHTIG (v1.0.1): Unter FreeRTOS laufen setup()/loop() in der Aufgabe
// "CORE0" des Cores, die nur 4 KB Stack hat. Die komplette Initialisierung
// (JSON, Netzwerk, TLS-Vorbereitung, Logausgaben) passt da nicht hinein -
// v1.0.0 lief deshalb beim Start in einen Stack-Ueberlauf und blieb haengen.
// Seit v1.0.1 erledigt eine eigene Aufgabe "main" mit 24 KB Stack alles;
// setup()/loop() tun praktisch nichts mehr.
// ---------------------------------------------------------------------
static void main_init() {
    statusled::begin(DD_PIN_LED);
    statusled::set(statusled::BOOT);
    delay(1500);   // Zeit fuer den USB-Seriell-Monitor
    Serial.println();
    Serial.println("==== " APP_NAME " v" APP_VERSION " startet ====");
    Serial.printf("Letzter Neustart: %s\n", plat::boot_reason().c_str());
    if (!LittleFS.begin()) {
        Serial.println("LittleFS wird formatiert ...");
        LittleFS.format();
        LittleFS.begin();
    }
    logf("[SYS] Start v%s - letzter Neustart: %s", APP_VERSION, plat::boot_reason().c_str());
    cfg::init();

    // Netzwerk-Einstellungen
    bool dhcp = true;
    std::string ip, mask, gw, dns, ntp, tz;
    {
        plat::Lock lk(cfg::mtx());
        JsonObject n = cfg::doc()["network"];
        dhcp = n["dhcp"] | true;
        ip = n["ip"] | "";
        mask = n["netmask"] | "255.255.255.0";
        gw = n["gateway"] | "";
        dns = n["dns"] | "";
        g_hostname = n["hostname"] | "drucker-dashboard";
        ntp = n["ntp_server"] | "pool.ntp.org";
        tz = n["timezone"] | "CET-1CEST,M3.5.0,M10.5.0/3";
    }
    setenv("TZ", tz.c_str(), 1);
    tzset();
    strncpy(wifi_station_hostname, g_hostname.c_str(), 31);

    // W5500 hart zuruecksetzen
    pinMode(PIN_ETH_RST, OUTPUT);
    digitalWrite(PIN_ETH_RST, LOW);
    delay(5);
    digitalWrite(PIN_ETH_RST, HIGH);
    delay(50);
    SPI.setRX(PIN_ETH_MISO);
    SPI.setCS(PIN_ETH_CS);
    SPI.setSCK(PIN_ETH_SCK);
    SPI.setTX(PIN_ETH_MOSI);
    eth.setSPISpeed(30000000);
    if (!dhcp && !ip.empty()) {
        IPAddress dnsA = ip_of(dns.c_str());
        if (dnsA == IPAddress(0, 0, 0, 0)) dnsA = ip_of(gw.c_str());
        eth.config(ip_of(ip.c_str()), ip_of(gw.c_str()), ip_of(mask.c_str()), dnsA);
    }
    lwipPollingPeriod(5);
    if (!eth.begin()) {
        logf("[NET] W5500 wurde nicht gefunden - Verkabelung/Board pruefen!");
        statusled::set(statusled::ERROR);
    } else {
        logf("[NET] W5500 gestartet, MAC %s", dev_mac().c_str());
    }
    // Die Abfrage-Aufgabe des Treibers laeuft mit Prioritaet 1 und wuerde von
    // den Drucker-Aufgaben (TLS-Rechnerei) ausgebremst -> hoeher setzen.
    if (TaskHandle_t ep = xTaskGetHandle("EthPoll")) vTaskPrioritySet(ep, configMAX_PRIORITIES - 3);
    logf("[NET] Warte auf Netzwerk (%s) ...", dhcp ? "DHCP" : "feste IP");
    uint32_t t0 = millis();
    while (!eth.connected() && millis() - t0 < 20000) delay(100);
    if (eth.connected()) {
        logf("[NET] IP-Adresse: %s  ->  http://%s/  (MAC %s)", dev_local_ip().c_str(), dev_local_ip().c_str(),
             dev_mac().c_str());
    } else {
        logf("[NET] Noch keine Netzwerkverbindung - Dienste starten trotzdem (DHCP laeuft weiter).");
    }
    NTP.begin(ntp.c_str());
    if (MDNS.begin(g_hostname.c_str())) {
        MDNS.addService("http", "tcp", 80);
        g_mdns = true;
        logf("[NET] mDNS: http://%s.local/", g_hostname.c_str());
    }
    app::start(80);
    g_started = true;
}

static void main_loop() {
    static uint32_t last_info = 0;
    static bool was_up = false;
    rp2040.wdt_reset();
    app::tick();
    if (g_mdns) MDNS.update();
    bool up = eth.connected();
    if (up != was_up) {
        was_up = up;
        if (up) logf("[NET] Netzwerk verbunden, IP %s", dev_local_ip().c_str());
        else logf("[NET] Netzwerkverbindung verloren.");
    }
    statusled::set(up ? statusled::OK : statusled::NO_NET);
    statusled::tick();
    if (millis() - last_info > 300000 || (last_info == 0 && millis() > 30000)) {
        last_info = millis();
        logf("[SYS] Freier Speicher: %u / %u Bytes", (unsigned)plat::free_heap(), (unsigned)plat::total_heap());
    }
}

static void main_task(void*) {
    main_init();
    rp2040.wdt_begin(8000);   // haengt die Hauptaufgabe > 8 s, startet das Board neu
    for (;;) {
        main_loop();
        delay(50);
    }
}

void setup() {
    Serial.begin(115200);
    TaskHandle_t h;
    xTaskCreate(main_task, "main", 24 * 1024 / sizeof(StackType_t), nullptr, tskIDLE_PRIORITY + 3, &h);
    vTaskCoreAffinitySet(h, 1 << 0);
}

void loop() {
    delay(1000);
}
