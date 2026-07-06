/* QEMU mps2-an385: 4 MB code SRAM at 0x0, 4 MB data SRAM at 0x20000000. */
MEMORY
{
  FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 4M
  RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 4M
}
_estack = ORIGIN(RAM) + LENGTH(RAM);

SECTIONS
{
  .text : {
    KEEP(*(.isr_vector))
    *(.text*) *(.rodata*)
    KEEP(*(.init)) KEEP(*(.fini))
    *(.ARM.extab*)
    . = ALIGN(4);
    __preinit_array_start = .; KEEP(*(.preinit_array)) __preinit_array_end = .;
    __init_array_start = .;  KEEP(*(.init_array*))   __init_array_end = .;
    __fini_array_start = .;  KEEP(*(.fini_array*))   __fini_array_end = .;
  } > FLASH

  .ARM.exidx : {
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
    . = ALIGN(4);
    _sidata = .;
  } > FLASH

  .data : AT(_sidata) {
    _sdata = .;
    *(.data*)
    . = ALIGN(4);
    _edata = .;
  } > RAM

  .bss : {
    _sbss = .;
    *(.bss*) *(COMMON)
    . = ALIGN(4);
    _ebss = .;
  } > RAM

  /* newlib heap grows from end; 128 KB reserved stack at the top */
  ._heap : { end = .; _end = .; __end__ = .; } > RAM
}
