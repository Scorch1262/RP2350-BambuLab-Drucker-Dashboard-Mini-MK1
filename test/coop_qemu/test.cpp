#include <Arduino.h>
#include <stdio.h>
#include <stdlib.h>
#include "coop.h"

extern "C" void delay(unsigned long ms);
static volatile uint32_t g_ms = 0;
extern "C" uint32_t millis() { return g_ms; }
extern "C" void sleep_ms(uint32_t ms) { uint32_t s = g_ms; while (g_ms - s < ms) {} }

// --- semihosting ---
static void sh_write0(const char* s) {
    register int r0 __asm("r0") = 4;
    register const char* r1 __asm("r1") = s;
    __asm volatile("bkpt 0xab" : "+r"(r0) : "r"(r1) : "memory");
}
static void out(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
#include <stdarg.h>
static void out(const char* fmt, ...) {
    char b[200];
    va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    sh_write0(b);
}
[[noreturn]] static void sh_exit(int code) {
    register int r0 __asm("r0") = 0x18;
    register int r1 __asm("r1") = code == 0 ? 0x20026 : 0x20024;
    __asm volatile("bkpt 0xab" : : "r"(r0), "r"(r1));
    for (;;) {}
}
extern "C" void watchdog_reboot(uint32_t, uint32_t, uint32_t) {
    out("REBOOT: %s in Aufgabe '%s'\n", g_dd_crash.what, g_dd_crash.task);
    sh_exit(g_dd_crash.what[0] == 'A' || g_dd_crash.what[0] == 'S' ? 0 : 1);
}
extern "C" void SysTick_Handler() {
    g_ms++;
    // etwas Stack im IRQ verbrauchen (simuliert lwIP im Interrupt)
    volatile uint32_t pad[64];
    for (int i = 0; i < 64; i++) pad[i] = i;
    (void)pad;
}

static volatile int g_fail = 0;
static volatile int g_done = 0;

static void ftask(void* arg) {
    int id = (int)(intptr_t)arg;
    float a = 1.0f + id, b = 0.5f * id;
    double acc = 0;
    uint32_t chk = 0;
    for (int i = 0; i < 2000; i++) {
        register float x = a * 1.0001f + b;   // FPU-Register ueber Umschaltung
        a = x;
        acc += a;
        chk += (uint32_t)i * id;
        if (i % 3 == 0) delay(0); else if (i % 7 == 0) delay(2); else coop::maybe_yield();
    }
    // Referenz ohne Umschaltung
    float ra = 1.0f + id, rb = 0.5f * id; double racc = 0; uint32_t rchk = 0;
    for (int i = 0; i < 2000; i++) { float x = ra * 1.0001f + rb; ra = x; racc += ra; rchk += (uint32_t)i * id; }
    if (ra != a || racc != acc || rchk != chk) { out("FEHLER Aufgabe %d: Registerinhalt verfaelscht\n", id); g_fail = 1; }
    else out("Aufgabe %d ok (acc=%d)\n", id, (int)acc);
    g_done++;
}

// Mutex-Test (gleiche Logik wie plat_arduino.cpp)
struct MtxState { coop::Task* owner; int count; };
static coop::Task* const kNoTask = (coop::Task*)1;
static coop::Task* me() { coop::Task* t = coop::current(); return t ? t : kNoTask; }
static void mlock(MtxState* s) { coop::Task* m = me(); while (s->count > 0 && s->owner != m) coop::sleep(0); s->owner = m; s->count++; }
static void munlock(MtxState* s) { if (s->count > 0 && --s->count == 0) s->owner = nullptr; }
static MtxState g_m{nullptr, 0};
static volatile int g_shared = 0;
static void mtask(void*) {
    for (int i = 0; i < 200; i++) {
        mlock(&g_m);
        mlock(&g_m);  // rekursiv
        int v = g_shared;
        delay(1);     // Umschalten waehrend gesperrt
        g_shared = v + 1;
        munlock(&g_m);
        munlock(&g_m);
        delay(0);
    }
    g_done++;
}

static int deep(int n) {
    volatile char buf[200];
    buf[0] = (char)n;
    if (n == 0) return buf[0];
    return deep(n - 1) + buf[0];
}
static void overflow_task(void*) {
    out("Starte Stack-Ueberlauf-Test ...\n");
    volatile int r = deep(100);   // ~20 KB > 4 KB Stack
    out("FEHLER: kein Absturz erkannt (%d)\n", r);
    sh_exit(1);
}
static int g_spawned = 0;
static void short_task(void*) { delay(1); g_spawned++; }

extern "C" int main() {
    *(volatile uint32_t*)0xE000ED88 |= (0xFu << 20);   // FPU an
    __asm volatile("dsb\nisb");
    *(volatile uint32_t*)0xE000E014 = 25000 - 1;        // SysTick 1 ms @25 MHz
    *(volatile uint32_t*)0xE000E018 = 0;
    *(volatile uint32_t*)0xE000E010 = 7;
    out("coop-Test startet\n");
    for (int i = 1; i <= 4; i++) coop::start("f", ftask, (void*)(intptr_t)i, 4096);
    coop::start("m1", mtask, nullptr, 4096);
    coop::start("m2", mtask, nullptr, 4096);
    coop::start("m3", mtask, nullptr, 4096);
    for (int i = 0; i < 50; i++) coop::start("s", short_task, nullptr, 2048);
    uint32_t t0 = g_ms;
    while (g_done < 7 || g_spawned < 50) {
        coop::run_once();
        if (g_ms - t0 > 20000) { out("FEHLER: Zeitueberschreitung\n"); sh_exit(1); }
    }
    out("Mutex-Zaehler: %d (erwartet 600), Aufgaben uebrig: %d, Kurzaufgaben: %d\n", g_shared, coop::count(), g_spawned);
    if (g_shared != 600 || coop::count() != 0) g_fail = 1;
    void* p = malloc(1000); out("Heap nach Aufraeumen ok: %p\n", p); free(p);
    if (g_fail) { out("TEST FEHLGESCHLAGEN\n"); sh_exit(1); }
    out("Funktionstests bestanden.\n");
    coop::start("ueberlauf", overflow_task, nullptr, 4096);
    for (;;) coop::run_once();
}
