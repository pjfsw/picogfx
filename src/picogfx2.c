#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "stdio.h"
#include "picogfx2_output.h"
#include "font.h"

#define VISIBLE_CHARS (VISIBLE_COLS>>3)

uint16_t* font16 = (uint16_t*)font;

char message1[] = "First layer";
char message2[] = "Second layer";
char message3[] = "Third layer";
char message4[] = "Fourth layer";

uint16_t shift_left[4] = {0,2,4,6};
uint8_t shift_right[4] = {8,6,4,2};
uint16_t right_mask[4] = {0x00,0x03,0x0f,0x3f};

static inline void render_line(int row) {
    uint8_t tile_offset = (row&7)<<1;
    uint16_t xpos = 0;
    uint16_t nibble = (xpos << 1) & 1;
    uint16_t offset = 0;

    char msg[] = "ABCD";

    for (int x = 0; x < VISIBLE_COLS/4; x++) {
        uint32_t fb = 0;

        uint16_t font_pos;
        uint8_t ch;
        ch = msg[((x+nibble)>>1)&3];
        font_pos = nibble + tile_offset + (ch*16);
        fb |= (font[font_pos] << shift_left[offset]);
        ch = msg[((x+nibble+1)>>1)&3];
        font_pos = nibble + 1 + tile_offset + (ch*16);
        fb |= (font[font_pos] >> shift_right[offset]) & right_mask[offset];
        nibble = (nibble + 1)&1;
        multicore_fifo_push_blocking(fb);

        /*uint8_t ls = layer_shift[0];
        uint16_t bmp = bitmap[0];
        fb |= ((bmp & mask[offset0]) >> shift_right[offset0]) << (shift_left[offset0] + ls);
        fb |= ((bmp & mask[offset0+1]) >> shift_right[offset0+1]) << (shift_left[offset0+1] + ls);
        fb |= ((bmp & mask[offset0+2]) >> shift_right[offset0+2]) << (shift_left[offset0+2] + ls);
        fb |= ((bmp & mask[offset0+3]) >> shift_right[offset0+3]) << (shift_left[offset0+3] + ls);
        offset0 = (offset0+4)&0x7;

        ls = layer_shift[1];
        bmp = bitmap[1];
        fb |= ((bmp & mask[offset1]) >> shift_right[offset1]) << (shift_left[offset1] + ls);
        fb |= ((bmp & mask[offset1+1]) >> shift_right[offset1+1]) << (shift_left[offset1+1] + ls);
        fb |= ((bmp & mask[offset1+2]) >> shift_right[offset1+2]) << (shift_left[offset1+2] + ls);
        fb |= ((bmp & mask[offset1+3]) >> shift_right[offset1+3]) << (shift_left[offset1+3] + ls);
        offset1 = (offset1+4)&0x7;*/

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
