// =====================================================================
// Kooperative Aufgaben (Koroutinen) fuer den RP2350 - ersetzt FreeRTOS.
//
// Hintergrund (v1.1.0): Mit "FreeRTOS SMP" aus arduino-pico 6.2.0 haengt
// auf dem RP2350-Mini-ETH bereits eth.begin() - das Board bekommt keine
// IP. Ohne FreeRTOS laeuft der W5500 einwandfrei (Netztest A). Daher laufen
// alle Aufgaben (Webserver-Verbindungen, Drucker, Kameras ...) jetzt als
// Koroutinen mit eigenem Stack auf Core 0 - dort, wo arduino-pico das
// Netzwerk erwartet. Umgeschaltet wird nur in delay()/yield(); diese
// rufen arduino-pico (WiFiClient, DNS, ...) beim Warten ohnehin auf.
// =====================================================================
#pragma once
#include <stdint.h>

// Absturz-Merker im nicht initialisierten RAM (ueberlebt den Neustart)
#define DD_CRASH_MAGIC 0x44444352u
struct DdCrashInfo { uint32_t magic; char what[24]; char task[24]; };
extern DdCrashInfo g_dd_crash;

namespace coop {
struct Task;
// Neue Aufgabe anlegen (Stack aus dem Heap). false bei Speichermangel.
bool start(const char* name, void (*fn)(void*), void* arg, uint32_t stack_bytes);
// Aus loop() aufrufen: laesst jede bereite Aufgabe einmal laufen.
void run_once();
// true, wenn der Aufrufer in einer Aufgabe laeuft (und umschalten darf)
bool in_task();
Task* current();
const char* current_name();
// Abgeben fuer mindestens ms Millisekunden (0 = nur kurz abgeben)
void sleep(uint32_t ms);
int count();
// Gibt ab, wenn die laufende Aufgabe schon laenger als ~20 ms rechnet
// (verhindert, dass ein schneller Datenstrom alle anderen ausbremst).
void maybe_yield();
// Laengste Zeit, die eine Aufgabe am Stueck lief (ohne abzugeben)
struct Stats {
    uint32_t max_block_ms;
    const char* max_block_task;
    uint32_t last_long_ms;        // letzte Blockade >= 250 ms
    const char* last_long_task;
    uint32_t long_count;          // Anzahl Blockaden >= 250 ms
};
Stats stats();
// Absturzinfo setzen (Name der laufenden Aufgabe) und neu starten
[[noreturn]] void crash_reboot(const char* what);
}
