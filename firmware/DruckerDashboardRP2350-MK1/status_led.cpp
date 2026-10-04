#ifdef ARDUINO
#include <Arduino.h>
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "ws2812.pio.h"
#include "status_led.h"

namespace statusled {

static PIO g_pio = nullptr;
static uint g_sm = 0;
static bool g_ok = false;
static State g_state = BOOT;
static uint32_t g_last = 0;
static int g_last_rgb = -1;

static void put_rgb(uint8_t r, uint8_t g, uint8_t b) {
    int v = (r << 16) | (g << 8) | b;
    if (!g_ok || v == g_last_rgb) return;
    g_last_rgb = v;
    // WS2812 erwartet GRB, MSB zuerst, linksbuendig in 32 Bit
    uint32_t grb = ((uint32_t)g << 16) | ((uint32_t)r << 8) | b;
    pio_sm_put_blocking(g_pio, g_sm, grb << 8u);
}

void begin(int pin) {
    uint offset;
    if (!pio_claim_free_sm_and_add_program_for_gpio_range(&ws2812_program, &g_pio, &g_sm, &offset, pin, 1, true)) return;
    ws2812_program_init(g_pio, g_sm, offset, pin, 800000, false);
    g_ok = true;
    put_rgb(0, 0, 0);
}

void set(State s) { g_state = s; }

void tick() {
    uint32_t t = millis();
    bool blink = (t / 500) % 2;
    switch (g_state) {
        case BOOT: put_rgb(0, 0, 24); break;
        case OK: put_rgb(0, 10, 0); break;
        case NO_NET: blink ? put_rgb(24, 8, 0) : put_rgb(0, 0, 0); break;
        case ERROR: put_rgb(32, 0, 0); break;
    }
    (void)g_last;
}

} // namespace statusled
#endif
