#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "stdio.h"
#include "picogfx2_output.h"
#include "font.h"

#define VISIBLE_CHARS (VISIBLE_COLS>>3)

uint16_t* font16 = (uint16_t*)font;

static inline void render_line(int row) {
    uint8_t tile_offset = row&7;
    uint16_t xpos = 0;
    for (int x = 0; x < VISIBLE_CHARS; x++) {
        uint16_t tile = 65;
        uint16_t bitmap = font16[tile*8+tile_offset];
        //uint16_t bitmap = 0b1100000011110101;
        uint32_t fb;
        fb =   bitmap & 0b0000000000000011;
        fb |= (bitmap & 0b0000000000001100) << 6;
        fb |= (bitmap & 0b0000000000110000) << 12;
        fb |= (bitmap & 0b0000000011000000) << 18;
        fb |= 0b100100001001000000010000;
        multicore_fifo_push_blocking(fb);
        fb =  (bitmap & 0b0000001100000000) >> 8;
        fb |= (bitmap & 0b0000110000000000) >> 2;
        fb |= (bitmap & 0b0011000000000000) << 4;
        fb |= (bitmap & 0b1100000000000000) << 10;
        //fb |= 0x10001000;
        multicore_fifo_push_blocking(fb);
    }
}

int main() {
    set_sys_clock_khz(270000, true);

    multicore_launch_core1(signal_generator);
    while (true) {
        for (int i = 0; i < VISIBLE_LINES; i++) {
            render_line(i);
        }
    }
}
