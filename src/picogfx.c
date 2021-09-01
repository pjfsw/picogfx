#include <string.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "vga.pio.h"
#include "spritedata.h"
#include "math.h"
#include "font.h"

#define HSYNC_BIT 6
#define VSYNC_BIT 7
#define DEBUG_PIN 7
#define SCRW 64
#define SCRH 512
#define NUMBER_OF_SPRITES 8
#define SPRITE_WIDTH 16
#define CHARS_PER_LINE 52
#define V_VISIBLE_AREA 600
#define LAST_VISIBLE_ROW 596
#define SPRITE_RIGHT_EDGE 416
#define ROWS_PER_FRAME 628
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
int dma_chan[4];
uint16_t current_dma;
uint16_t next_row;
uint16_t pixel_row;
// We probably[tm] won't use higher resolutions than 800x600
uint8_t framebuffer[4][512];
uint8_t syncbuffer[2][256];
uint8_t framebuffer_index[ROWS_PER_FRAME];
uint8_t syncbuffer_index[ROWS_PER_FRAME];

uint8_t scrollPos;
uint8_t colors[] = {0,0};
uint16_t *color16;

typedef struct {
    uint8_t screen[SCRW*SCRH];
    uint8_t colorMem[SCRW*SCRH];
    uint16_t spriteX[NUMBER_OF_SPRITES];
    uint16_t spriteY[NUMBER_OF_SPRITES];
    uint8_t spriteHeight[NUMBER_OF_SPRITES];
    uint16_t palettes[256];
    uint8_t mode;   // 0 - 50x36 textmode, 1 - bitmap mode
    uint16_t scrollY;
    uint16_t scrollX;
    uint8_t font[2048];

} Vram;

Vram vram;
uint8_t dummy[131702-sizeof(Vram)];
uint8_t *vramBytes = (uint8_t*)&vram;

uint32_t nextPtr = 0;

// VRAM

uint8_t spritePos[NUMBER_OF_SPRITES];

const int PIXEL_BUFFER_0 = 0;
const int PIXEL_BUFFER_1 = 1;
const int PIXEL_BUFFER_PORCH = 2;
const int PIXEL_BUFFER_SYNC = 3;
const int SYNC_BUFFER = 0;
const int SYNC_BUFFER_VSYNC = 1;

uint8_t frameCounter = 0;
uint8_t palette[] = {3, 12, 48, 63};
int16_t sinTable[256];
int16_t cosTable[256];

uint16_t get_length(Timing *timing) {
    return timing->visible_area + timing->front_porch + timing->sync_pulse + timing->back_porch;
}

void set_800_600(HVTiming *vga_timing) {
    vga_timing->h.visible_area = 800/SCALE;
    vga_timing->h.front_porch = 40/SCALE;
    vga_timing->h.sync_pulse = 128/SCALE;
    vga_timing->h.back_porch = 88/SCALE;
    vga_timing->h.length = get_length(&vga_timing->h);
    vga_timing->v.visible_area = 600;
    vga_timing->v.front_porch = 1;
    vga_timing->v.sync_pulse = 4;
    vga_timing->v.back_porch = 23;
    vga_timing->v.length = get_length(&vga_timing->v);
    vga_timing->pixel_clock = 40000000.0/(float)SCALE;
}


void init_sync_buffer(uint8_t buffer[], Timing *t, uint8_t vsync_mask) {
    uint8_t hsync_mask = 1 << HSYNC_BIT;
    uint16_t pos = 0;

    for (int i = 0; i < t->front_porch; i++) {
        buffer[pos++] = vsync_mask;
    }
    for (int i = 0; i < t->sync_pulse; i++) {
        buffer[pos++] = hsync_mask | vsync_mask;
    }
    for (int i = 0; i < t->back_porch; i++) {
        buffer[pos++] = vsync_mask;
    }
}

int isDebug() {
    gpio_init(DEBUG_PIN);
    gpio_set_dir(DEBUG_PIN, GPIO_IN);
    return gpio_get(DEBUG_PIN);
}

static inline void draw_sprites(uint8_t *fb) {
    for (int i = 0; i < NUMBER_OF_SPRITES; i++) {
        int16_t spritePos = pixel_row-vram.spriteY[i];
        if (spritePos < 0 || spritePos > vram.spriteHeight[i]) {
            continue;
        }
        spritePos = spritePos << 4;

        int n = SPRITE_WIDTH;
        uint16_t xPos = vram.spriteX[i];
        if (xPos > SPRITE_RIGHT_EDGE) {
            n = SPRITE_RIGHT_EDGE-xPos;
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

static inline void draw_bitmap(uint8_t *fb) {
    uint16_t yOffset = ((pixel_row + vram.scrollY) & 0x1ff) << 6;
    uint16_t xOffset = ((vram.scrollX & 0x1ff) >> 3);
    uint8_t *scrPos = &vram.screen[yOffset];
    uint8_t *colorPos = &vram.colorMem[yOffset];
    uint8_t c;
    uint16_t xpos = (7-vram.scrollX) & 7;

    for (uint8_t tile = 0; tile < CHARS_PER_LINE; tile++) {
        *color16 = vram.palettes[colorPos[xOffset]] & 0x3F3F;
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

static inline void draw_text(uint8_t *fb) {
    uint16_t yRow = (pixel_row + vram.scrollY) & 0x1ff;
    uint16_t yOffset = (yRow >> 3) << 6;
    uint16_t yMod = yRow & 7;
    uint16_t xOffset = ((vram.scrollX & 0x1ff) >> 3);
    uint8_t *scrPos = &vram.screen[yOffset];
    uint8_t *colorPos = &vram.colorMem[yOffset];
    uint8_t c;
    uint16_t xpos = (7-vram.scrollX) & 7;

    for (uint8_t tile = 0; tile < CHARS_PER_LINE; tile++) {
        *color16 = vram.palettes[colorPos[xOffset]] & 0x3F3F;
        c = vram.font[(scrPos[xOffset] << 3) + yMod];
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

static inline void pixel_dma_handler() {
    // The chained DMA has already started the next row when IRQ happens, so we need to increase
    // the counter here to be in sync with what we are setting up for next IRQ
    next_row = (next_row + 1) % vga_timing.v.length;
    pixel_row = next_row >> 1;

    uint8_t *fb = framebuffer[framebuffer_index[next_row]];
    dma_channel_set_read_addr(dma_chan[current_dma], fb+8, false);

    if (next_row&1) {
        return;
    }
    if (next_row == 0) {
        frameCounter++;
        scrollPos++;
        vram.scrollX = 0;
        vram.scrollY = 0;
        //vram.scrollY = sinTable[scrollPos++];
        //vram.scrollX++;
        for (int i = 0 ; i < NUMBER_OF_SPRITES; i++) {
            //spriteX[i]++;
            spritePos[i]++;
            vram.spriteY[i] = sinTable[spritePos[i]];
            vram.spriteX[i] = cosTable[spritePos[i]];
        }
    }
    vram.mode = (next_row >> 7) &1;
    if (next_row < LAST_VISIBLE_ROW) {
        if (vram.mode) {
            draw_bitmap(fb);
        } else {
            draw_text(fb);
        }
        draw_sprites(fb);
    }
}

static inline void sync_dma_handler() {
    uint8_t *sb = syncbuffer[syncbuffer_index[next_row]];
    dma_channel_set_read_addr(dma_chan[current_dma], sb, false);
}

void dma_handler() {
    // Clear the interrupt request
    dma_hw->ints0 = 1u << dma_chan[current_dma];
    if (current_dma&1) {
        sync_dma_handler();
    } else {
        pixel_dma_handler();
    }
    current_dma = (current_dma + 1) & 3;
}

void init_buffers() {
    uint8_t vsync_on_mask = 1 << VSYNC_BIT;
    uint8_t vsync_off_mask = 0;
    // Three framebuffers are for visible display (tripple buffering) + one with black pixels
    for (int i = 0; i < 3; i++) {
        memset(framebuffer[i], 0, 512);
    }
    memset(framebuffer[PIXEL_BUFFER_SYNC], vsync_on_mask, 512);

    // Vertical blank buffer, no pixels
    init_sync_buffer(syncbuffer[SYNC_BUFFER], &vga_timing.h, vsync_off_mask);
    // Vertical sync buffer, no pixels
    init_sync_buffer(syncbuffer[SYNC_BUFFER_VSYNC], &vga_timing.h, vsync_on_mask);

    uint16_t pos = 0;
    for (int i = 0; i < vga_timing.v.visible_area; i++) {
        syncbuffer_index[pos] = SYNC_BUFFER;
        framebuffer_index[pos] = (pos>>1)&1;
        pos++;
    }
    for (int i = 0; i < vga_timing.v.front_porch; i++) {
        syncbuffer_index[pos] = SYNC_BUFFER;
        framebuffer_index[pos] = PIXEL_BUFFER_PORCH;
        pos++;
    }
    for (int i = 0; i < vga_timing.v.sync_pulse; i++) {
        syncbuffer_index[pos] = SYNC_BUFFER_VSYNC;
        framebuffer_index[pos] = PIXEL_BUFFER_SYNC;
        pos++;
    }
    for (int i = 0; i < vga_timing.v.back_porch; i++) {
        syncbuffer_index[pos] = SYNC_BUFFER;
        framebuffer_index[pos] = PIXEL_BUFFER_PORCH;
        pos++;
    }
}

void init_app_stuff() {
    for (int i = 0; i < 256; i++) {
        sinTable[i] = 150 + 140 * sin(i * M_PI / 128);
        cosTable[i] = 208 + 208 * cos(i * M_PI / 128);
    }
    const int xOffset = (400-NUMBER_OF_SPRITES*16)/2;
    for (int i = 0; i < NUMBER_OF_SPRITES; i++) {
        spritePos[i] = i << 2;
        vram.spriteHeight[i] = 24;
    }
    for (int i = 0; i < 256; i++) {
        vram.palettes[i] = ((i&63)<<8) | (i>>2);
    }
    uint8_t palette = 0;
    uint8_t character = 32;
    for (int i = 0; i < SCRW*SCRH; i++) {
        vram.screen[i] = character++;
        vram.colorMem[i] = palette++;
        if (character > 128) {
            character = 32;
        }
    }

}

void gpio_irq_handler(uint gpio, uint32_t events) {
    gpio_acknowledge_irq(gpio, events);
    uint32_t v = gpio_get_all();
    vramBytes[nextPtr] = v & 255;
    nextPtr = (nextPtr + 1) % 131072;
}


void init_vram() {
    color16 = (uint16_t*)colors;
    memcpy(vram.font, font, 2048);
}

void boot_loader() {
    // TODO add code to populate RAM memory
}

int main() {
    boot_loader();
    init_vram();
    int debug = isDebug();

    set_sys_clock_khz(270000, true);

    set_800_600(&vga_timing);
    init_buffers();

    PIO pio = pio0;
    const uint8_t VGA_BASE_PIN = 8;

    uint offset = pio_add_program(pio, &vga_program);
    uint sm = pio_claim_unused_sm(pio, true);
    float freq = vga_timing.pixel_clock;
    float div = debug ? 65535 : (clock_get_hz(clk_sys) / freq);

    init_app_stuff();
    vga_program_init(pio, sm, offset, VGA_BASE_PIN, div);

    const int framebuffer_words = vga_timing.h.visible_area/4;
    const int sync_words = get_length(&vga_timing.h)/4 - framebuffer_words;
    dma_chan[0] = dma_claim_unused_channel(true);
    dma_chan[1] = dma_claim_unused_channel(true);
    dma_chan[2] = dma_claim_unused_channel(true);
    dma_chan[3] = dma_claim_unused_channel(true);
    for (int i = 0; i < 4; i+=2) {
        // Setup pixel data DMA (0, 2)
        dma_channel_config c = dma_channel_get_default_config(dma_chan[i]);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
        channel_config_set_read_increment(&c, true);
        channel_config_set_dreq(&c, DREQ_PIO0_TX0);
        channel_config_set_chain_to(&c, dma_chan[i+1]);
        dma_channel_configure(dma_chan[i], &c, &pio0_hw->txf[0], framebuffer[i>>1], framebuffer_words, false);
        dma_channel_set_irq0_enabled(dma_chan[i], true);

        // Setup sync data DMA (1, 3)
        c = dma_channel_get_default_config(dma_chan[i+1]);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
        channel_config_set_read_increment(&c, true);
        channel_config_set_dreq(&c, DREQ_PIO0_TX0);
        channel_config_set_chain_to(&c, dma_chan[(i+2)%4]);
        dma_channel_configure(dma_chan[i+1], &c, &pio0_hw->txf[0], syncbuffer[0], sync_words, false);
        dma_channel_set_irq0_enabled(dma_chan[i+1], true);
    }
    irq_set_exclusive_handler(DMA_IRQ_0, dma_handler);
    irq_set_enabled(DMA_IRQ_0, true);
    next_row = 1;
    current_dma = 0;
    dma_start_channel_mask(1u << dma_chan[0]);
    const int positiveEdge = 1 << 3;
    gpio_set_irq_enabled_with_callback(DEBUG_PIN, positiveEdge, true, gpio_irq_handler);
    while (true) {
    }
}
