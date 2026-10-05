#pragma once
#include <stdint.h>
#include <string.h>
#define __uninitialized_ram(x) __attribute__((section(".noinit"))) x
extern "C" uint32_t millis();
extern "C" void sleep_ms(uint32_t ms);
static inline unsigned get_core_num() { return 0; }
namespace std {}
