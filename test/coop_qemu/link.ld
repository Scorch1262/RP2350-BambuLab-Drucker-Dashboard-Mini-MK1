MEMORY { CODE (rx) : ORIGIN = 0x10000000, LENGTH = 1M  RAM (rwx) : ORIGIN = 0x38000000, LENGTH = 1M }
SECTIONS {
  .text : { KEEP(*(.vectors)) *(.text*) *(.rodata*) . = ALIGN(4);
    __preinit_array_start = .; KEEP(*(.preinit_array)) __preinit_array_end = .;
    __init_array_start = .; KEEP(*(.init_array*)) __init_array_end = .; *(.init) *(.fini) } > CODE
  .ARM.exidx : { *(.ARM.exidx*) } > CODE
  __data_load__ = LOADADDR(.data);
  .data : { __data_start__ = .; *(.data*) . = ALIGN(4); __data_end__ = .; } > RAM AT > CODE
  .bss (NOLOAD) : { __bss_start__ = .; *(.bss*) *(COMMON) . = ALIGN(4); __bss_end__ = .; } > RAM
  .noinit (NOLOAD) : { *(.noinit*) } > RAM
  . = ALIGN(8); end = .; __end__ = .;
  __stack_top = ORIGIN(RAM) + LENGTH(RAM);
}
