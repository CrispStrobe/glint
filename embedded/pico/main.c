/* glint benchmark entry for RP2040 (see CMakeLists.txt). */
#include <stdint.h>
#include <stdio.h>
#include "pico/stdlib.h"

extern int bench_main(void);

uint64_t bench_now_us(void) { return time_us_64(); }
int bench_write_files(void) { return 0; }

int main(void) {
    stdio_init_all();
    sleep_ms(3000);  /* give the USB serial console time to attach */
    for (;;) {
        bench_main();
        sleep_ms(5000);
    }
}
