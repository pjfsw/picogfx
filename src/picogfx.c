//#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "vga.pio.h"
#include "font.h"

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

uint16_t last_visible_row = 0;
uint16_t half_width;
HVTiming vga_timing;
int dma_chan[2];
uint16_t current_dma;
uint16_t next_row;
uint8_t framebuffer_index[628];
// We probably[tm] won't use higher resolutions than 800x600
uint8_t framebuffer[5][1056];

const int PIXEL_BUFFER_0 = 0;
const int PIXEL_BUFFER_1 = 1;
const int VBLANK_BUFFER = 2;
const int VSYNC_BUFFER = 3;
const int PIXEL_BUFFER_2 = 4; // Special buffer :)

char message[] = "ABCDCCCC";
uint8_t frameCounter = 0;
uint8_t screen[50*37];

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
    half_width = vga_timing->h.visible_area/2;
    last_visible_row = vga_timing->v.visible_area & 0xfff0; // Don't want to show half a tile row
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

    uint8_t *fb = framebuffer[framebuffer_index[next_row]];
    dma_channel_set_read_addr(dma_chan[current_dma], fb, false);
    current_dma = 1-current_dma;

    if (next_row == 0) {
        frameCounter++;
    }
    if (next_row < last_visible_row) {
        uint8_t scrollX = frameCounter&7;
        uint16_t xpos = (next_row & 1) * half_width;
        uint8_t tile_row = (next_row >> 1) & 7;
        uint8_t colors[] = {0,63};
        int x;
        uint8_t tile;
        uint8_t shift = scrollX;
        for (int x = 0; x < half_width; x++) {
            tile = (x >> 3);
            shift = (shift - 1) & 7;
            fb[xpos++] = colors[(font[65][tile_row] >> shift) & 1];
        }
    }
}

void init_frame_buffers() {
    uint8_t vsync_on_mask = 1 << VSYNC_BIT;
    uint8_t vsync_off_mask = 0;
    // First two framebuffers are for visible display (double buffering)
    init_row((uint32_t*)framebuffer[0], &vga_timing.h, vsync_off_mask);
    init_row((uint32_t*)framebuffer[1], &vga_timing.h, vsync_off_mask);
    init_row((uint32_t*)framebuffer[4], &vga_timing.h, vsync_off_mask); // Blank but not vblank
    // Vertical blank buffer, no pixels
    init_row((uint32_t*)framebuffer[2], &vga_timing.h, vsync_off_mask);
    // Vertical sync buffer, no pixels
    init_row((uint32_t*)framebuffer[3], &vga_timing.h, vsync_on_mask);

    int row = 0;
    for (int i = 0; i < last_visible_row; i++) {
        framebuffer_index[row] = (row >> 1) % 2;
        row++;
    }
    for (int i = last_visible_row; i < vga_timing.v.visible_area; i++) {
        framebuffer_index[row++] = PIXEL_BUFFER_2;
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
    set_sys_clock_khz(180000, true);
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

    uint16_t columns = get_length(&vga_timing.h);

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
}
