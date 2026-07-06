/* glint benchmark entry for ESP-IDF (see ../CMakeLists.txt). */
#include <stdint.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern int bench_main(void);

uint64_t bench_now_us(void) { return (uint64_t)esp_timer_get_time(); }
int bench_write_files(void) { return 0; }

void app_main(void) {
    for (;;) {
        bench_main();
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
