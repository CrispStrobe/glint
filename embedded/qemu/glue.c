/* QEMU semihosting glue: no useful clock (QEMU is not cycle-accurate),
 * but semihosted file IO lets the host decode-validate the output. */
#include <stdint.h>
uint64_t bench_now_us(void) { return 0; }
int bench_write_files(void) { return 1; }
