#include <stdint.h>
extern uint32_t __stack_top, __data_start__, __data_end__, __data_load__, __bss_start__, __bss_end__;
extern int main(void);
void SysTick_Handler(void);
void isr_hardfault(void);
extern void __libc_init_array(void);
void Reset_Handler(void) {
    uint32_t *s = &__data_load__, *d = &__data_start__;
    while (d < &__data_end__) *d++ = *s++;
    for (d = &__bss_start__; d < &__bss_end__;) *d++ = 0;
    __asm volatile("msr msplim, %0" :: "r"((uint32_t)&__stack_top - 8192));   // wie Bootrom: Grenze gesetzt
    __libc_init_array();
    main();
    for (;;) {}
}
static void Default_Handler(void) { for (;;) {} }
__attribute__((section(".vectors"), used)) const void* vectors[16] = {
    &__stack_top, Reset_Handler, Default_Handler, isr_hardfault, isr_hardfault, isr_hardfault, isr_hardfault,
    0, 0, 0, 0, Default_Handler, Default_Handler, 0, Default_Handler, SysTick_Handler };
