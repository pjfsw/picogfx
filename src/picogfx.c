//#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "vga.pio.h"

#define HSYNC_BIT 6
#define VSYNC_BIT 7

typedef struct {
    uint16_t visible_area;
    uint16_t front_porch;
    uint16_t sync_pulse;
    uint16_t back_porch;
} Timing;

typedef struct {
    Timing h;
    Timing v;
} HVTiming;

void set_800_600(HVTiming *vga_timing, int scale) {
    vga_timing->h.visible_area = 800/scale;
    vga_timing->h.front_porch = 40/scale;
    vga_timing->h.sync_pulse = 128/scale;
    vga_timing->h.back_porch = 88/scale;
    vga_timing->v.visible_area = 600;
    vga_timing->v.front_porch = 1;
    vga_timing->v.sync_pulse = 4;
    vga_timing->v.back_porch = 23;
}

uint16_t get_length(Timing *timing) {
    return timing->visible_area + timing->front_porch + timing->sync_pulse + timing->back_porch;
}

void init_row(uint32_t row[], Timing *t, uint8_t pixel_mask, uint8_t vsync_mask) {
    uint16_t pos = 0;
    uint8_t rgb = 0;
    uint32_t v = 0;
    uint32_t vsync_mask32 = vsync_mask | (vsync_mask << 8) | (vsync_mask << 16) | (vsync_mask << 24);

    for (int i = 0; i < t->visible_area; i++) {
        v = v >> 8;
        v |= (rgb & pixel_mask) << 24;
        if (i % 4 == 3) {
            row[pos++] = v | vsync_mask32;
            v = 0;
        }
        rgb++;
    }
    for (int i = 0; i < t->front_porch/4; i++) {
        row[pos++] = vsync_mask32;
    }
    uint8_t hsync_mask = 1 << HSYNC_BIT;
    uint32_t hsync_mask32 = hsync_mask | (hsync_mask << 8) | (hsync_mask << 16) | (hsync_mask << 24);
    for (int i = 0; i < t->sync_pulse/4; i++) {
        row[pos++] = hsync_mask32 | vsync_mask32;
    }
    for (int i = 0; i < t->back_porch/4; i++) {
        row[pos++] = vsync_mask32;
    }
}

int main() {
    const int scale = 2;

    HVTiming vga_timing;
    set_800_600(&vga_timing, scale);
    uint16_t columns = get_length(&vga_timing.h);
    uint16_t rows = get_length(&vga_timing.v);


    const uint8_t visibleRow = 0;
    const uint8_t invisibleRow = 1;
    const uint8_t vsyncRow = 2;
    uint32_t row[3][columns/4];
    init_row(row[0], &vga_timing.h, 63, 0);
    init_row(row[1], &vga_timing.h, 0, 0);
    init_row(row[2], &vga_timing.h, 0, (1 << VSYNC_BIT));

    uint8_t row_def[rows];
    uint16_t pos = 0;
    for (int i = 0; i < vga_timing.v.visible_area; i++) {
        row_def[pos++] = visibleRow;
    }
    for (int i = 0; i < vga_timing.v.front_porch; i++) {
        row_def[pos++] = invisibleRow;
    }
    for (int i = 0; i < vga_timing.v.sync_pulse; i++) {
        row_def[pos++] = vsyncRow;
    }
    for (int i = 0; i < vga_timing.v.back_porch; i++) {
        row_def[pos++] = invisibleRow;
    }

    PIO pio = pio0;
    const uint8_t VGA_BASE_PIN = 8;

    uint offset = pio_add_program(pio, &vga_program);
    uint sm = pio_claim_unused_sm(pio, true);
    float freq = 20000000;
    float div = clock_get_hz(clk_sys) / freq;
    //float div = 10000;
    vga_program_init(pio, sm, offset, VGA_BASE_PIN, div);

    while (true) {
        for (uint16_t y = 0 ; y < rows; y++) {
            uint8_t row_type = row_def[y];
            uint32_t *scanline = row[row_type];
            for (uint16_t x = 0; x < columns/4; x++) {
                pio_sm_put_blocking(pio, sm, scanline[x]);
            }
        }
    }
}
