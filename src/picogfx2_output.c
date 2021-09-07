#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "stdio.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "vga.pio.h"
#include "picogfx2_output.h"

#define VGA_BASE_PIN 0
#define HSYNC_PIN (VGA_BASE_PIN+6)
#define VGA_CLOCK 12577000

#define HFRONT 8
#define HSYNC 48
#define BFRONT 24
#define VSYNC_MASK 0x80808080
#define HSYNC_MASK 0x40404040
#define SYNC_MASK  0xc0c0c0c0

PIO vga_pio;
uint vga_sm;

uint32_t framebuffer[320];
bool odd_line;

uint32_t colors32 = 0x3f0c0300;
uint8_t *colors = (uint8_t*)&colors32;
uint8_t demux_table[256];

static inline uint32_t demultiplex(uint32_t pixels) {
    return colors[demux_table[pixels & 0xff]]
    | (colors[demux_table[(pixels >> 8) & 0xff]] << 8)
    | (colors[demux_table[(pixels >> 16) & 0xff]] << 16)
    | (colors[demux_table[(pixels >> 24) & 0xff]] << 24);
}

static inline void vga_line() {
    for (uint32_t i = 0; i < VISIBLE_COLS/4; i++) {
        uint32_t color = demultiplex(multicore_fifo_pop_blocking()) & 0x3f3f3f3f;
        framebuffer[i] = color;
        pio_sm_put_blocking(vga_pio, vga_sm, color | SYNC_MASK);
    }
}

static void inline vga_line_buffered() {
    for (uint32_t i = 0; i < VISIBLE_COLS/4; i++) {
        int rgb = framebuffer[i];
        pio_sm_put_blocking(vga_pio, vga_sm, SYNC_MASK | (rgb&0x3f3f3f3f));
    }
}

static void inline vga_line_sync() {
    for (uint32_t i = 0; i < HFRONT/4; i++) {
        pio_sm_put_blocking(vga_pio, vga_sm, SYNC_MASK);
    }
    for (uint32_t i = 0; i < HSYNC/4; i++) {
        pio_sm_put_blocking(vga_pio, vga_sm, VSYNC_MASK);
    }
    for (uint32_t i = 0; i < BFRONT/4; i++) {
        pio_sm_put_blocking(vga_pio, vga_sm, SYNC_MASK);
    }
}

static void inline vblank_line(uint32_t vsync) {
    for (uint32_t i = 0; i < (VISIBLE_COLS+HFRONT)/4; i++) {
        pio_sm_put_blocking(vga_pio, vga_sm, HSYNC_MASK | vsync);
    }
    for (uint32_t i = 0; i < HSYNC/4; i++) {
        pio_sm_put_blocking(vga_pio, vga_sm, vsync);
    }
    for (uint32_t i = 0; i < BFRONT/4; i++) {
        pio_sm_put_blocking(vga_pio, vga_sm, HSYNC_MASK | vsync);
    }
}

void init_demultiplexing_table() {
    for (int i = 0; i < 256; i++) {
        if (i < 4) {
            demux_table[i] = i;
        } else if (i < 16) {
            demux_table[i] = i >> 2;
        } else if (i < 64) {
            demux_table[i] = i >> 4;
        } else {
            demux_table[i] = i >> 6;
        }
    }
}

void signal_generator() {
    init_demultiplexing_table();

    vga_pio = pio0;
    uint vga_offset = pio_add_program(vga_pio, &vga_program);
    vga_sm = pio_claim_unused_sm(vga_pio, true);

    for (int i = 0; i < 8; i++) {
        pio_gpio_init(vga_pio, VGA_BASE_PIN+i);
    }

    float freq = VGA_CLOCK;
    float clock_div = clock_get_hz(clk_sys) / freq;
    vga_program_init(vga_pio, vga_sm, vga_offset, VGA_BASE_PIN, clock_div);

    while (true) {
        for (int i = 0; i < VISIBLE_LINES; i++) {
            vga_line();
            vga_line_sync();
            vga_line_buffered();
            vga_line_sync();
        }
        for (int i = 0; i < 10; i++) {
            vblank_line(VSYNC_MASK);
        }
        for (int i = 0; i < 2; i++) {
            vblank_line(0);
        }
        for (int i = 0; i < 33; i++) {
            vblank_line(VSYNC_MASK);
        }
    }
}
