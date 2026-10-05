#ifdef ARDUINO
#include <Arduino.h>
#include <hardware/watchdog.h>
#include <hardware/sync.h>
#include <string.h>
#include <stdlib.h>
#include <new>
#include "coop.h"

DdCrashInfo __uninitialized_ram(g_dd_crash);

namespace coop {

#define STACK_GUARD 256          // Reserve unter der Stack-Grenze (MSPLIM)
#define CANARY 0xC0DEFACEu

struct Task {
    const char* name;
    uint32_t* sp;
    uint8_t* stack;
    uint32_t size;
    void (*fn)(void*);
    void* arg;
    uint32_t wake;
    uint32_t slice;
    bool done;
    Task* next;
};

static Task* g_tasks = nullptr;
}  // namespace coop
// fuer den HardFault-Handler (Assembler) sichtbar
extern "C" {
coop::Task* coop_cur = nullptr;
uint32_t* coop_sched_sp = nullptr;
}
namespace coop {
#define g_cur coop_cur
#define g_sched_sp coop_sched_sp
static uint32_t g_sched_limit = 0;
static bool g_limit_read = false;
static int g_count = 0;

// Kontextwechsel: sichert r3-r11, lr, s16-s31 auf dem alten Stack, merkt
// den Stackzeiger in *save, wechselt auf den neuen Stack und setzt die
// Stack-Grenze (MSPLIM) - ein Ueberlauf loest damit sofort einen Fehler aus.
extern "C" __attribute__((naked, noinline)) void coop_swap(uint32_t** save, uint32_t* to, uint32_t limit) {
    __asm volatile(
        "push {r3-r11, lr}\n"
        "vpush {s16-s31}\n"
        "mov r3, sp\n"
        "str r3, [r0]\n"
        "movs r3, #0\n"
        "msr msplim, r3\n"
        "mov sp, r1\n"
        "msr msplim, r2\n"
        "vpop {s16-s31}\n"
        "pop {r3-r11, pc}\n");
}

static uint32_t ipsr() {
    uint32_t v;
    __asm volatile("mrs %0, ipsr" : "=r"(v));
    return v;
}

bool in_task() { return g_cur != nullptr && get_core_num() == 0 && ipsr() == 0; }
Task* current() { return in_task() ? g_cur : nullptr; }
const char* current_name() { return g_cur ? g_cur->name : "main"; }
int count() { return g_count; }

static void entry() {
    Task* t = g_cur;
    t->fn(t->arg);
    t->done = true;
    coop_swap(&t->sp, g_sched_sp, g_sched_limit);
    for (;;) {}
}

bool start(const char* name, void (*fn)(void*), void* arg, uint32_t stack_bytes) {
    if (stack_bytes < 2048) stack_bytes = 2048;
    stack_bytes = (stack_bytes + 7) & ~7u;
    Task* t = new (std::nothrow) Task();
    if (!t) return false;
    t->stack = (uint8_t*)malloc(stack_bytes);
    if (!t->stack) {
        delete t;
        return false;
    }
    t->name = name;
    t->size = stack_bytes;
    t->fn = fn;
    t->arg = arg;
    t->wake = 0;
    t->done = false;
    *(uint32_t*)t->stack = CANARY;
    uint32_t* sp = (uint32_t*)(((uintptr_t)t->stack + stack_bytes) & ~7u);
    *--sp = (uint32_t)(uintptr_t)&entry;   // pc (Thumb-Bit ist gesetzt)
    for (int i = 0; i < 9; i++) *--sp = 0; // r3-r11
    for (int i = 0; i < 16; i++) *--sp = 0; // s16-s31
    t->sp = sp;
    t->next = nullptr;
    // hinten anhaengen (Reihenfolge = Start-Reihenfolge)
    Task** pp = &g_tasks;
    while (*pp) pp = &(*pp)->next;
    *pp = t;
    g_count++;
    return true;
}

void sleep(uint32_t ms) {
    if (!in_task()) {
        if (ms) sleep_ms(ms);
        return;
    }
    Task* t = g_cur;
    t->wake = ::millis() + ms;
    coop_swap(&t->sp, g_sched_sp, g_sched_limit);
}

void maybe_yield() {
    if (in_task() && ::millis() - g_cur->slice >= 20) sleep(0);
}

[[noreturn]] void crash_reboot(const char* what) {
    g_dd_crash.magic = DD_CRASH_MAGIC;
    strncpy(g_dd_crash.what, what, sizeof(g_dd_crash.what) - 1);
    g_dd_crash.what[sizeof(g_dd_crash.what) - 1] = 0;
    strncpy(g_dd_crash.task, g_cur ? g_cur->name : "main", sizeof(g_dd_crash.task) - 1);
    g_dd_crash.task[sizeof(g_dd_crash.task) - 1] = 0;
    watchdog_reboot(0, 0, 10);
    for (;;) {}
}

void run_once() {
    if (!g_limit_read) {
        __asm volatile("mrs %0, msplim" : "=r"(g_sched_limit));
        g_limit_read = true;
    }
    uint32_t now = ::millis();
    Task** pp = &g_tasks;
    while (*pp) {
        Task* t = *pp;
        if (!t->done && (int32_t)(now - t->wake) >= 0) {
            g_cur = t;
            t->slice = ::millis();
            coop_swap(&g_sched_sp, t->sp, (uint32_t)(uintptr_t)t->stack + STACK_GUARD);
            g_cur = nullptr;
            if (*(uint32_t*)t->stack != CANARY) {
                g_cur = t;
                crash_reboot("Stack-Ueberlauf");
            }
            now = ::millis();
        }
        if (t->done) {
            *pp = t->next;
            free(t->stack);
            delete t;
            g_count--;
            continue;
        }
        pp = &t->next;
    }
}

} // namespace coop

// ---- delay()/yield() von arduino-pico ersetzen: in einer Aufgabe wird
// waehrenddessen auf die anderen Aufgaben umgeschaltet. -------------------
extern "C" void delay(unsigned long ms) {
    if (coop::in_task()) {
        coop::sleep(ms);
        return;
    }
    if (ms) sleep_ms(ms);
}

extern "C" void yield() {
    if (coop::in_task()) coop::sleep(0);
}

// ---- Absturz (HardFault, z. B. Stack-Ueberlauf ueber MSPLIM) -> Neustart
extern "C" void dd_hardfault_c() { coop::crash_reboot("Absturz (HardFault)"); }
// Lief eine Aufgabe, ist ihr Stack evtl. uebergelaufen -> auf den freien
// Bereich unterhalb des Verteiler-Stacks wechseln, bevor C-Code laeuft.
extern "C" __attribute__((naked)) void isr_hardfault() {
    __asm volatile(
        "movs r0, #0\n"
        "msr msplim, r0\n"
        "ldr r0, =coop_cur\n"
        "ldr r0, [r0]\n"
        "cbz r0, 1f\n"
        "ldr r0, =coop_sched_sp\n"
        "ldr r0, [r0]\n"
        "cbz r0, 1f\n"
        "subs r0, #16\n"
        "bic r0, r0, #7\n"
        "mov sp, r0\n"
        "1:\n"
        "b dd_hardfault_c\n"
        ".ltorg\n");
}
#endif
