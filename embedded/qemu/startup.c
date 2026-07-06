/* Minimal Cortex-M startup for the QEMU mps2-an385 semihosted benchmark. */
#include <stdint.h>

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;
extern void initialise_monitor_handles(void);
extern void __libc_init_array(void);
extern int bench_main(void);
extern void exit(int);

void Reset_Handler(void) {
    uint32_t *src = &_sidata, *dst = &_sdata;
    while (dst < &_edata) *dst++ = *src++;
    for (dst = &_sbss; dst < &_ebss;) *dst++ = 0;
    initialise_monitor_handles();
    __libc_init_array();
    exit(bench_main());
}

void Default_Handler(void) { for (;;) {} }

__attribute__((section(".isr_vector"), used))
const void* vector_table[] = {
    &_estack,
    (void*)Reset_Handler,
    (void*)Default_Handler, (void*)Default_Handler, (void*)Default_Handler,
    (void*)Default_Handler, (void*)Default_Handler, 0, 0, 0, 0,
    (void*)Default_Handler, 0, 0,
    (void*)Default_Handler, (void*)Default_Handler,
};
