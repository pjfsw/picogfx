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
    uint8_t positive_pulse;
} Timing;

typedef struct {
    Timing h;
    Timing v;
    float pixel_clock;
    Uint8_t pixel_allowed_value;
} HVTiming;

void set_800_600(HVTiming *vga_timing, int scale) {
    vga_timing->h.visible_area = 800/scale;
    vga_timing->h.front_porch = 40/scale;
    vga_timing->h.sync_pulse = 128/scale;
    vga_timing->h.back_porch = 88/scale;
    vga_timing->h.positive_pulse = 1;
    vga_timing->v.visible_area = 600;
    vga_timing->v.front_porch = 1;
    vga_timing->v.sync_pulse = 4;
    vga_timing->v.back_porch = 23;
    vga_timing->v.positive_pulse = 1;
    vga_timing->v.pixel_allowed_value = 0;
    vga_timing->pixel_clock = 40000000.0/(float)scale;
}

void set_640_480(HVTiming *vga_timing, int scale) {
    vga_timing->h.visible_area = 640/scale;
    vga_timing->h.front_porch = 16/scale;
    vga_timing->h.sync_pulse = 96/scale;
    vga_timing->h.back_porch = 48/scale;
    vga_timing->h.positive_pulse = 0;
    vga_timing->v.visible_area = 480;
    vga_timing->v.front_porch = 10;
    vga_timing->v.sync_pulse = 2;
    vga_timing->v.back_porch = 33;
    vga_timing->v.positive_pulse = 0;
    vga_timing->v.pixel_allowed_value = 192;
    vga_timing->pixel_clock = 25000000.0/(float)scale;
}

uint16_t get_length(Timing *timing) {
    return timing->visible_area + timing->front_porch + timing->sync_pulse + timing->back_porch;
}

void init_row(uint32_t row[], Timing *t, uint8_t vsync_mask) {
    uint8_t hsync_on_mask = 1 << HSYNC_BIT;
    uint8_t hsync_off_mask = 0;
    if (!t->positive_pulse) {
        hsync_off_mask = hsync_on_mask;
        hsync_on_mask = 0;
    }

    uint16_t pos = 0;
    for (int i = 0; i < t->visible_area; i++) {
        row[pos++] = hsync_off_mask | vsync_mask;
        }
    }
    for (int i = 0; i < t->front_porch; i++) {
        row[pos++] = hsync_off_mask | vsync_mask;
    }
    for (int i = 0; i < t->sync_pulse; i++) {
        row[pos++] = hsync_on_mask | vsync_mask;
    }
    for (int i = 0; i < t->back_porch; i++) {
        row[pos++] = hsync_off_mask | vsync_mask;
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
    uint8_t row[3][columns];
    uint8_t vsync_on_mask = 1 << VSYNC_BIT;
    uint8_t vsync_off_mask = 0;
    if (!vga_timing.v.positive_pulse) {
        vsync_off_mask = vsync_on_mask;
        vsync_on_mask = 0;
    }

    init_row(row[0], &vga_timing.h, 63, vsync_off_mask);
    init_row(row[1], &vga_timing.h, 0, vsync_off_mask);
    init_row(row[2], &vga_timing.h, 0, vsync_on_mask);

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
    float freq = vga_timing.pixel_clock;
    float div = clock_get_hz(clk_sys) / freq;
    //float div = 10000;
    vga_program_init(pio, sm, offset, VGA_BASE_PIN, div);

    uint32_t framebuffer_size = 400*300;
    uint8_t framebuffer[framebuffer_size];
    for (int i = 0; i < framebuffer_size; i++) {
        framebuffer[i] = i & 63;
    }

    uint32_t v = 0;
    while (true) {
        for (uint16_t y = 0 ; y < rows; y++) {
            uint8_t row_type = row_def[y];
            uint8_t *sync = row[row_type];
            for (uint16_t x = 0; x < columns; x++) {
                v = sync[x] | (sync[x+1] << 8) | (sync[x+2] << 16) | (sync[x+3] << 24);
                if (sync[x] == vga_timing.pixel_allowed_value) {
                    v|= framebuffer[x] | (framebuffer[x+1] << 8) | (framebuffer[x+2] << 16) | (framebuffer[x+3] << 24);
                }
                pio_sm_put_blocking(pio, sm, v);
            }
        }
    }
}
