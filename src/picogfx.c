//#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "vga.pio.h"

#define HSYNC_BIT 6
#define VSYNC_BIT 7
#define DEBUG_PIN 7

typedef struct {
    uint16_t visible_area;
    uint16_t front_porch;
    uint16_t sync_pulse;
    uint16_t back_porch;
    uint16_t length;
} Timing;

typedef struct {
    Timing h;
    Timing v;
    float pixel_clock;
} HVTiming;

HVTiming vga_timing;
int dma_chan[2];
uint16_t current_dma;
uint16_t next_row;
uint8_t framebuffer_index[628];
// We probably[tm] won't use higher resolutions than 800x600
uint8_t framebuffer[4][1056];

const int PIXEL_BUFFER_0 = 0;
const int PIXEL_BUFFER_1 = 1;
const int VBLANK_BUFFER = 2;
const int VSYNC_BUFFER = 3;


uint16_t get_length(Timing *timing) {
    return timing->visible_area + timing->front_porch + timing->sync_pulse + timing->back_porch;
}

void set_800_600(HVTiming *vga_timing, int scale) {
    vga_timing->h.visible_area = 800/scale;
    vga_timing->h.front_porch = 40/scale;
    vga_timing->h.sync_pulse = 128/scale;
    vga_timing->h.back_porch = 88/scale;
    vga_timing->h.length = get_length(&vga_timing->h);
    vga_timing->v.visible_area = 600;
    vga_timing->v.front_porch = 1;
    vga_timing->v.sync_pulse = 4;
    vga_timing->v.back_porch = 23;
    vga_timing->v.length = get_length(&vga_timing->v);
    vga_timing->pixel_clock = 40000000.0/(float)scale;
}


void init_row(uint32_t row[], Timing *t, uint8_t vsync_mask) {
    uint16_t pos = 0;
    uint32_t vsync_mask32 = vsync_mask | (vsync_mask << 8) | (vsync_mask << 16) | (vsync_mask << 24);

    uint8_t hsync_mask = 1 << HSYNC_BIT;
    uint32_t hsync_mask32 = hsync_mask | (hsync_mask << 8) | (hsync_mask << 16) | (hsync_mask << 24);

    for (int i = 0; i < t->visible_area/4; i++) {
        row[pos++] = vsync_mask32;
    }
    for (int i = 0; i < t->front_porch/4; i++) {
        row[pos++] = vsync_mask32;
    }
    for (int i = 0; i < t->sync_pulse/4; i++) {
        row[pos++] = hsync_mask32 | vsync_mask32;
    }
    for (int i = 0; i < t->back_porch/4; i++) {
        row[pos++] = vsync_mask32;
    }
}

int isDebug() {
    gpio_init(DEBUG_PIN);
    gpio_set_dir(DEBUG_PIN, GPIO_IN);
    return gpio_get(DEBUG_PIN);
}

void dma_handler() {
    // Clear the interrupt request
    dma_hw->ints0 = 1u << dma_chan[current_dma];
    // The chained DMA has already started the next row when IRQ happens, so we need to increase
    // the counter here to be in sync with what we are setting up for next IRQ
    next_row = (next_row + 1) % vga_timing.v.length;

    dma_channel_set_read_addr(dma_chan[current_dma], framebuffer[framebuffer_index[next_row]], false);
    current_dma = 1-current_dma;
}

void init_frame_buffers() {
    uint8_t vsync_on_mask = 1 << VSYNC_BIT;
    uint8_t vsync_off_mask = 0;
    // First two framebuffers are for visible display (double buffering)
    init_row((uint32_t*)framebuffer[0], &vga_timing.h, vsync_off_mask);
    init_row((uint32_t*)framebuffer[1], &vga_timing.h, vsync_off_mask);
    // Vertical blank buffer, no pixels
    init_row((uint32_t*)framebuffer[2], &vga_timing.h, vsync_off_mask);
    // Vertical sync buffer, no pixels
    init_row((uint32_t*)framebuffer[3], &vga_timing.h, vsync_on_mask);

    int row = 0;
    for (int i = 0; i < vga_timing.v.visible_area; i++) {
        framebuffer_index[row] = (row >> 1) % 2;
        row++;
    }
    for (int i = 0; i < vga_timing.v.front_porch; i++) {
        framebuffer_index[row++] = VBLANK_BUFFER;
    }
    for (int i = 0; i < vga_timing.v.sync_pulse; i++) {
        framebuffer_index[row++] = VSYNC_BUFFER;
    }
    for (int i = 0; i < vga_timing.v.back_porch; i++) {
        framebuffer_index[row++] = VBLANK_BUFFER;
    }
}

int main() {
//    set_sys_clock_khz(160000, true);
//    clock_configure(clk_sys, 0, 0, 0, 140000000);
    const int scale = 2;
    int debug = isDebug();

    set_800_600(&vga_timing, scale);
    init_frame_buffers();

    PIO pio = pio0;
    const uint8_t VGA_BASE_PIN = 8;

    uint offset = pio_add_program(pio, &vga_program);
    uint sm = pio_claim_unused_sm(pio, true);
    float freq = vga_timing.pixel_clock;
    float div = debug ? 65535 : (clock_get_hz(clk_sys) / freq);

    vga_program_init(pio, sm, offset, VGA_BASE_PIN, div);

    for (int i = 0; i < vga_timing.h.visible_area; i++) {
        framebuffer[0][i] = i & 63;
        framebuffer[1][i] = ((i>>4) % 2) ? 0b00101010 : 0b00010101;
    }

    uint16_t columns = get_length(&vga_timing.h);
    //uint16_t rows = get_length(&vga_timing.v);

    dma_chan[0] = dma_claim_unused_channel(true);
    dma_chan[1] = dma_claim_unused_channel(true);
    for (int i = 0; i < 2; i++) {
        dma_channel_config c = dma_channel_get_default_config(dma_chan[i]);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
        channel_config_set_read_increment(&c, true);
        channel_config_set_dreq(&c, DREQ_PIO0_TX0);
        channel_config_set_chain_to(&c, dma_chan[1-i]);
        dma_channel_configure(dma_chan[i], &c, &pio0_hw->txf[0], framebuffer[framebuffer_index[i]], columns/4, false);
        dma_channel_set_irq0_enabled(dma_chan[i], true);
        irq_set_exclusive_handler(DMA_IRQ_0, dma_handler);
        irq_set_enabled(DMA_IRQ_0, true);
    }
    current_dma = 0;
    next_row = 1;
    dma_start_channel_mask(1u << dma_chan[0]);

    while (true) {
        // Here be sprites and stuff
    }


/*
    uint32_t framebuffer_size = 400*300;
    uint8_t framebuffer[framebuffer_size];
    for (int i = 0; i < framebuffer_size; i++) {
        uint8_t rgb = i & 63;
        framebuffer[i] = rgb;
    }

    uint32_t vx = vga_timing.h.visible_area / 4;
    uint32_t cx = columns / 4;
    uint32_t *framebuffer32 = (uint32_t*)framebuffer;

    uint32_t fbpos = 0;
    while (true) {
        for (uint16_t y = 0 ; y < vga_timing.v.visible_area; y++) {
            fbpos = y>>1;
            for (uint16_t x = 0; x < vx; x++) {
                pio_sm_put_blocking(pio, sm, framebuffer32[fbpos]);
                fbpos++;
            }
            for (uint16_t x = vx; x < cx; x++) {
                pio_sm_put_blocking(pio, sm, row[0][x]);
            }
        }
        for (uint16_t y = vga_timing.v.visible_area ; y < rows; y++) {
            uint8_t row_type = row_def[y];
            uint32_t *sync = row[row_type];
            for (uint16_t x = 0; x < cx; x++) {
                pio_sm_put_blocking(pio, sm, sync[x]);
            }
        }
    }*/
}
