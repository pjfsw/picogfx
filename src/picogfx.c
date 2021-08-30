//#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "vga.pio.h"
#include "font.h"
#include "spritedata.h"
#include "math.h"

#define HSYNC_BIT 6
#define VSYNC_BIT 7
#define DEBUG_PIN 7
#define SCRW 64
#define SCRH 512
#define NUMBER_OF_SPRITES 8
#define SPRITE_WIDTH 16
#define CHARS_PER_LINE 51
#define FRAMEBUFFER_OFFSET 112
#define V_VISIBLE_AREA 600
#define LAST_VISIBLE_ROW 596
#define SCALE 2

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
uint16_t pixel_row;
uint8_t framebuffer_index[628];
// We probably[tm] won't use higher resolutions than 800x600
uint8_t framebuffer[5][2048];
uint16_t scrollY;
uint16_t scrollX;
uint8_t scrollPos;
uint8_t colors[] = {0,0};
uint16_t *color16;

// VRAM
uint8_t screen[SCRW*SCRH];
uint8_t colorMem[SCRW*SCRH];
uint16_t spriteX[NUMBER_OF_SPRITES];
uint16_t spriteY[NUMBER_OF_SPRITES];
uint16_t palettes[256];

uint8_t spritePos[NUMBER_OF_SPRITES];

const int PIXEL_BUFFER_0 = 0;
const int PIXEL_BUFFER_1 = 1;
const int BLACK_BUFFER = 2; // Special buffer :)
const int VBLANK_BUFFER = 3;
const int VSYNC_BUFFER = 4;

uint8_t frameCounter = 0;
uint8_t palette[] = {3, 12, 48, 63};
uint16_t sinTable[256];
uint16_t cosTable[256];

uint16_t get_length(Timing *timing) {
    return timing->visible_area + timing->front_porch + timing->sync_pulse + timing->back_porch;
}

void set_800_600(HVTiming *vga_timing) {
    vga_timing->h.visible_area = 800/SCALE;
    vga_timing->h.front_porch = 40/SCALE;
    vga_timing->h.sync_pulse = 128/SCALE;
    vga_timing->h.back_porch = 88/SCALE;
    vga_timing->h.length = get_length(&vga_timing->h);
    vga_timing->v.visible_area = V_VISIBLE_AREA;
    vga_timing->v.front_porch = 1;
    vga_timing->v.sync_pulse = 4;
    vga_timing->v.back_porch = 23;
    vga_timing->v.length = get_length(&vga_timing->v);
    vga_timing->pixel_clock = 40000000.0/(float)SCALE;
}


void init_row(uint32_t row[], Timing *t, uint8_t vsync_mask) {
    uint16_t pos = FRAMEBUFFER_OFFSET/4;
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

static inline void draw_sprites(uint8_t *fb) {
    for (int i = 0; i < NUMBER_OF_SPRITES; i++) {
        int16_t spritePos = pixel_row-spriteY[i];
        if (spritePos < 0 || spritePos > 15) {
            continue;
        }
        spritePos = spritePos << 4;

        int n = SPRITE_WIDTH;
        uint16_t xPos = spriteX[i];
        if (xPos > vga_timing.h.visible_area-SPRITE_WIDTH) {
            n = vga_timing.h.visible_area-xPos;
        }

        for (int x = 0; x < n; x++) {
            uint8_t c = spritedata[spritePos++];
            if ((c & 0x80) == 0) {
                fb[xPos] = c & 63;
            }
            xPos++;
        }
    }
}

static inline void draw_tiles(uint8_t *fb) {
    uint16_t yOffset = ((pixel_row + scrollY) & 0x1ff) << 6;
    uint16_t xOffset = ((scrollX & 0x1ff) >> 3);
    uint8_t *scrPos = &screen[yOffset];
    uint8_t *colorPos = &colorMem[yOffset];
    uint8_t c;
    uint16_t xpos = (7-scrollX) & 7;

    for (uint8_t tile = 0; tile < CHARS_PER_LINE; tile++) {
        *color16 = palettes[colorPos[xOffset]] & 0x3F3F;
        c = scrPos[xOffset];
        fb[xpos+7] = colors[c&1];
        c = c >> 1;
        fb[xpos+6] = colors[c&1];
        c = c >> 1;
        fb[xpos+5] = colors[c&1];
        c = c >> 1;
        fb[xpos+4] = colors[c&1];
        c = c >> 1;
        fb[xpos+3] = colors[c&1];
        c = c >> 1;
        fb[xpos+2] = colors[c&1];
        c = c >> 1;
        fb[xpos+1] = colors[c&1];
        c = c >> 1;
        fb[xpos] = colors[c&1];
        xpos+=8;
        xOffset = (xOffset + 1) & 63;
    }
}

void dma_handler() {
    // Clear the interrupt request
    dma_hw->ints0 = 1u << dma_chan[current_dma];
    // The chained DMA has already started the next row when IRQ happens, so we need to increase
    // the counter here to be in sync with what we are setting up for next IRQ
    next_row = (next_row + 1) % vga_timing.v.length;
    pixel_row = next_row >> 1;

    uint8_t *fb = &framebuffer[framebuffer_index[next_row]][FRAMEBUFFER_OFFSET];
    dma_channel_set_read_addr(dma_chan[current_dma], fb, false);
    current_dma = 1-current_dma;
    fb-=8; // To allow for horisontal scrolling

    if (next_row == 0) {
        frameCounter++;
        scrollPos++;
        scrollY = sinTable[scrollPos++];
        scrollX++;
        for (int i = 0 ; i < NUMBER_OF_SPRITES; i++) {
            //spriteX[i]++;
            spritePos[i]++;
            spriteY[i] = 276; // sinTable[spritePos[i]];
            spriteX[i] = cosTable[spritePos[i]];
        }
    }
    if (next_row < LAST_VISIBLE_ROW) {
        if (!(next_row & 1)) {
            draw_tiles(fb);
            draw_sprites(fb);
        }
    }
}

void init_frame_buffers() {
    uint8_t vsync_on_mask = 1 << VSYNC_BIT;
    uint8_t vsync_off_mask = 0;
    // Three framebuffers are for visible display (tripple buffering) + one with black pixels
    for (int i = 0; i < 3; i++) {
        init_row((uint32_t*)framebuffer[i], &vga_timing.h, vsync_off_mask);
    }
    // Vertical blank buffer, no pixels
    init_row((uint32_t*)framebuffer[VBLANK_BUFFER], &vga_timing.h, vsync_off_mask);
    // Vertical sync buffer, no pixels
    init_row((uint32_t*)framebuffer[VSYNC_BUFFER], &vga_timing.h, vsync_on_mask);

    int row = 0;
    for (int i = 0; i < LAST_VISIBLE_ROW; i++) {
        framebuffer_index[row] = (row >> 1) % 2;
        row++;
    }
    for (int i = LAST_VISIBLE_ROW; i < vga_timing.v.visible_area; i++) {
        framebuffer_index[row++] = BLACK_BUFFER;
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

void init_app_stuff() {
    for (int i = 0; i < 256; i++) {
        sinTable[i] = 140 + 140 * sin(i * M_PI / 128);
        cosTable[i] = 193 + 192 * cos(i * M_PI / 128);
    }
    const int xOffset = (400-NUMBER_OF_SPRITES*16)/2;
    for (int i = 0; i < NUMBER_OF_SPRITES; i++) {
        spritePos[i] = i << 2;
    }
    for (int i = 0; i < 256; i++) {
        palettes[i] = ((i&63)<<8) | (i>>2);
    }
    uint8_t character = 32;
    uint8_t palette = 0;
    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 64; x++) {
            for (int f = 0; f < 8; f++) {
                int pos = (y*8+f)*SCRW+x;
                screen[pos] = font[character][f];
                colorMem[pos] = palette++;
            }
            character = character + 1;
            if (character > 128) {
                character = 32;
            }
        }
    }

}

void init_vram() {
    color16 = (uint16_t*)colors;
}

int main() {
    set_sys_clock_khz(270000, true);
    init_vram();

    int debug = isDebug();

    set_800_600(&vga_timing);
    init_frame_buffers();

    PIO pio = pio0;
    const uint8_t VGA_BASE_PIN = 8;

    uint offset = pio_add_program(pio, &vga_program);
    uint sm = pio_claim_unused_sm(pio, true);
    float freq = vga_timing.pixel_clock;
    float div = debug ? 65535 : (clock_get_hz(clk_sys) / freq);

    init_app_stuff();
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
        dma_channel_configure(dma_chan[i], &c, &pio0_hw->txf[0], &framebuffer[framebuffer_index[i]][FRAMEBUFFER_OFFSET], columns/4, false);
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
