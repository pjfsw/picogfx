#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "math.h"

int16_t sinTable[256];
int16_t cosTable[256];

void core2() {
    uint32_t pos = 0;
    while (true) {
        pos++;
        multicore_fifo_push_blocking(pos);
        sleep_ms(10);
    }
}
