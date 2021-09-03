#include <string.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "vga.pio.h"
#include "databus.pio.h"
#include "spritedata.h"
#include "math.h"
#include "font.h"

#define HSYNC_BIT 6
#define VSYNC_BIT 7
//#define DEBUG_PIN 7
#define SCRW 64
#define SCRH 512
#define NUMBER_OF_SPRITES 8
#define SPRITE_WIDTH 16
#define CHARS_PER_LINE 52
#define V_VISIBLE_AREA 600
#define LAST_VISIBLE_ROW 592
#define SPRITE_RIGHT_EDGE 416
#define ROWS_PER_FRAME 628
#define FRAME_BUFFER_SIZE 512
#define VGA_BASE_PIN 0
#define DATABUS_BASE_PIN 8

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

HVTiming vga_timing = {
    .h = {
        .visible_area = 400,
        .front_porch = 20,
        .sync_pulse = 64,
        .back_porch = 44
    },
    .v = {
        .visible_area = 600,
        .front_porch = 1,
        .sync_pulse = 4,
        .back_porch = 23
    }
};

typedef struct {
    PIO pio;
    uint sm;
    uint offset;
} PioConf;

PioConf pio_conf;

int dma_chan[4];
uint16_t current_dma;
uint16_t next_row;
uint16_t pixel_row;
uint8_t syncbuffer[2][256];
uint8_t framebuffer_index[ROWS_PER_FRAME];
uint8_t syncbuffer_index[ROWS_PER_FRAME];

uint8_t scrollPos;
uint8_t colors[] = {0,0,0,0};
uint16_t *color16;
uint32_t *color32;

// We probably[tm] won't use higher resolutions than 800x600
uint8_t framebuffer[4][FRAME_BUFFER_SIZE];

typedef struct {
    uint8_t screen[32768];                  // 0x00000
    uint32_t colorMem[4096];                // 0x08000 - 4 unique colours per character
    uint8_t font[4096];                     // 0x0c000 - 4 colour font
    uint16_t spriteX[NUMBER_OF_SPRITES];    // 0x0d000
    uint16_t spriteY[NUMBER_OF_SPRITES];    // 0x0d010
    uint8_t spriteHeight[NUMBER_OF_SPRITES];// 0x0d020
    uint8_t spritePointer[NUMBER_OF_SPRITES];//0x0d028
    uint16_t scrollY;                       // 0x0d030
    uint16_t scrollX;                       // 0x0d032
    uint8_t bitmapStart;                    // 0x0d034
    uint8_t bitmapHeight;                   // 0x0d035
    uint16_t bitmapPtr;                     // 0x0d036 = Pointer in gfx mem in 2 byte blocks
    uint32_t bitmapPalette;                 // 0x0d038
    // 0x10000-0x1ffff Sprite data
} Vram;

uint8_t *spritePtr;
uint8_t vramBytes[0x20000];
Vram *vram;

bool debugMode;
uint32_t nextPtr = 0;

// VRAM

uint8_t spritePos[NUMBER_OF_SPRITES];

const int PIXEL_BUFFER_0 = 0;
const int PIXEL_BUFFER_1 = 1;
const int PIXEL_BUFFER_PORCH = 2;
const int PIXEL_BUFFER_SYNC = 3;
const int SYNC_BUFFER = 0;
const int SYNC_BUFFER_VSYNC = 1;

uint16_t frameCounter = 0;
int16_t sinTable[256];
int16_t cosTable[256];

uint16_t get_length(Timing *timing) {
    return timing->visible_area + timing->front_porch + timing->sync_pulse + timing->back_porch;
}

void set_800_600() {
    vga_timing.h.length = get_length(&vga_timing.h);
    vga_timing.v.length = get_length(&vga_timing.v);
    vga_timing.pixel_clock = 20000000.0;
}

void configure_and_start_dma(PioConf *pio_conf) {
    set_800_600();
    const int framebuffer_words = vga_timing.h.visible_area >> 2;
    const int sync_words = get_length(&vga_timing.h)/4 - framebuffer_words;

    float freq = vga_timing.pixel_clock;
    float clock_div = debugMode ? 65535 : (clock_get_hz(clk_sys) / freq);

    vga_program_init(pio_conf->pio, pio_conf->sm, pio_conf->offset, VGA_BASE_PIN, clock_div);
    for (int i = 0; i < 4; i+=2) {
        // Setup pixel data DMA (0, 2)
        dma_channel_config c = dma_channel_get_default_config(dma_chan[i]);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
        channel_config_set_read_increment(&c, true);
        channel_config_set_dreq(&c, DREQ_PIO0_TX0);
        channel_config_set_chain_to(&c, dma_chan[i+1]);
        dma_channel_configure(dma_chan[i], &c, &pio0_hw->txf[0], framebuffer[i>>2], framebuffer_words, false);
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

    next_row = 1;
    current_dma = 0;
    dma_start_channel_mask(1u << dma_chan[0]);
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

static inline void draw_sprites(uint8_t *fb) {
    for (int i = 0; i < NUMBER_OF_SPRITES; i++) {
        int16_t spritePos = pixel_row-vram->spriteY[i];
        if (spritePos < 0 || spritePos > vram->spriteHeight[i]) {
            continue;
        }
        uint8_t *sprite = &spritePtr[vram->spritePointer[i] << 8];
        spritePos = spritePos << 4;

        int n = SPRITE_WIDTH;
        uint16_t xPos = vram->spriteX[i];
        if (xPos > SPRITE_RIGHT_EDGE) {
            n = SPRITE_RIGHT_EDGE-xPos;
        }

        for (int x = 0; x < n; x++) {
            uint8_t c = sprite[spritePos++];
            if ((c & 0x80) == 0) {
                fb[xPos] = c & 63;
            }
            xPos++;
        }
    }
}

static inline void draw_bitmap(uint8_t *fb) {
    uint16_t bitmapRow = (pixel_row - vram->bitmapStart) & 0x1ff;
    uint16_t yOffset = bitmapRow << 6;
    uint8_t *scrPos = &vramBytes[((vram->bitmapPtr << 1) + yOffset) & 0x1ffff];
    uint16_t xpos = 0;
    *color32 = vram->bitmapPalette & 0x3f3f3f3f;
    uint8_t c;
    for (uint8_t tile = 0; tile < CHARS_PER_LINE; tile++) {
        c = scrPos[tile&63];
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
        //xOffset = (xOffset + 1) & 63;
    }
}

static inline void draw_tiles(uint8_t *fb) {
    uint16_t yRow = (pixel_row + vram->scrollY) & 0x1ff;
    uint16_t yOffset = (yRow >> 3) << 6;
    uint16_t yMod = (yRow & 7) << 1;
    uint16_t xOffset = ((vram->scrollX & 0x1ff) >> 3);
    uint8_t *scrPos = &vram->screen[yOffset];
    uint32_t *colorPos = &vram->colorMem[yOffset];
    uint8_t c;
    uint16_t xpos = (7-vram->scrollX) & 7;

    for (uint8_t tile = 0; tile < CHARS_PER_LINE; tile++) {
        uint16_t font_offset = yMod + (scrPos[xOffset] << 4);
        *color32 = colorPos[xOffset] & 0x3f3f3f3f;
        c = vram->font[font_offset];
        fb[xpos+3] = colors[c&3];
        c = c >> 2;
        fb[xpos+2] = colors[c&3];
        c = c >> 2;
        fb[xpos+1] = colors[c&3];
        c = c >> 2;
        fb[xpos] = colors[c&3];

        c = vram->font[font_offset + 1];
        fb[xpos+7] = colors[c&3];
        c = c >> 2;
        fb[xpos+6] = colors[c&3];
        c = c >> 2;
        fb[xpos+5] = colors[c&3];
        c = c >> 2;
        fb[xpos+4] = colors[c&3];
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
        vram->bitmapStart = frameCounter;
        //scrollPos++;
        //vram->scrollX++;
        //vram->scrollY++;
        //vram->mode = ((frameCounter >> 9) & 1);
        //vram.scrollY = sinTable[scrollPos++];
        //vram.scrollX++;
        for (int i = 0 ; i < NUMBER_OF_SPRITES; i++) {
            //spriteX[i]++;
            spritePos[i]++;
            vram->spriteY[i] = sinTable[spritePos[i]];
            vram->spriteX[i] = cosTable[spritePos[i]];
        }
    }
    if (next_row < LAST_VISIBLE_ROW) {
        if (pixel_row >= vram->bitmapStart && pixel_row < vram->bitmapStart + vram->bitmapHeight) {
            draw_bitmap(fb);
        } else {
            draw_tiles(fb);
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
    // Two framebuffers are for visible display (double buffering) + one with black pixels
    for (int i = 0; i < 2; i++) {
        memset(framebuffer[i], 0, FRAME_BUFFER_SIZE);
    }
    memset(framebuffer[PIXEL_BUFFER_SYNC], vsync_on_mask, FRAME_BUFFER_SIZE);

    // Vertical blank buffer, no pixels
    init_sync_buffer(syncbuffer[SYNC_BUFFER], &vga_timing.h, vsync_off_mask);
    // Vertical sync buffer, no pixels
    init_sync_buffer(syncbuffer[SYNC_BUFFER_VSYNC], &vga_timing.h, vsync_on_mask);

    uint16_t pos = 0;
    for (int i = 0; i < LAST_VISIBLE_ROW; i++) {
        syncbuffer_index[pos] = SYNC_BUFFER;
        framebuffer_index[pos] = (pos>>1)&1;
        pos++;
    }
    for (int i = LAST_VISIBLE_ROW; i < vga_timing.v.visible_area; i++) {
        syncbuffer_index[pos] = SYNC_BUFFER;
        framebuffer_index[pos] = PIXEL_BUFFER_PORCH;
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
        vram->spriteHeight[i] = 24;
    }
    uint8_t c = 0;
    for (int i = 0; i < 4096; i++) {
        uint32_t c32 = 0;
        for (int j = 0; j < 3; j++) {
            c32 = c32 | ((c++) & 63);
            c32 = c32 << 8;
        }

        vram->colorMem[i] = c32;
    }
    uint8_t palette = 0;
    uint8_t character = 32;
    for (int i = 0; i < 32768; i++) {
        vram->screen[i] = character++;
        if (character > 254) {
            character = 32;
        }
    }
    const int spriteTarget = 0;
    memcpy(&spritePtr[spriteTarget << 8], spritedata, sizeof(spritedata));
    for (int i = 0; i < NUMBER_OF_SPRITES; i++) {
        vram->spritePointer[i] = spriteTarget;
    }
    const int bitmap_height = 64;
    vram->bitmapPtr = 0x8100;
    vram->bitmapStart = 32;
    vram->bitmapHeight = bitmap_height;
    vram->bitmapPalette = 0b00111111001010100001010100000000;
    for (int y = 0; y < bitmap_height; y++) {
        for (int x = 0; x < 64; x++) {
            vramBytes[(vram->bitmapPtr << 1) + y * 64 + x] = 0xaa;
        }
    }

}

void init_vram() {
    color16 = (uint16_t*)colors;
    color32 = (uint32_t*)colors;
    vram = (Vram*)vramBytes;
    spritePtr = &vramBytes[0x10000];
    memcpy(vram->font, font, 4096);
}

void boot_loader() {
    // TODO add code to populate RAM memory
}

int main() {
    boot_loader();
    init_vram();
    //debugMode = isDebug();
    debugMode = false;

    set_sys_clock_khz(270000, true);

    init_buffers();
    init_app_stuff();

    const PIO databus_pio = pio1;
    const uint databus_offset = pio_add_program(databus_pio, &databus_program);
    const uint databus_sm = pio_claim_unused_sm(databus_pio, true);
    for (int i = 0; i < 11; i++) {
        pio_gpio_init(databus_pio, DATABUS_BASE_PIN+i);
    }

    float databus_clock_div = debugMode ? 65535 : (clock_get_hz(clk_sys) / 40000000.0);
    databus_program_init(databus_pio, databus_sm, databus_offset, DATABUS_BASE_PIN, databus_clock_div);

    pio_conf.pio = pio0;
    pio_conf.offset = pio_add_program(pio_conf.pio, &vga_program);
    pio_conf.sm = pio_claim_unused_sm(pio_conf.pio, true);

    for (int i = 0; i < 8; i++) {
        pio_gpio_init(pio_conf.pio, VGA_BASE_PIN+i);
    }

    dma_chan[0] = dma_claim_unused_channel(true);
    dma_chan[1] = dma_claim_unused_channel(true);
    dma_chan[2] = dma_claim_unused_channel(true);
    dma_chan[3] = dma_claim_unused_channel(true);
    configure_and_start_dma(&pio_conf);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_handler);
    irq_set_enabled(DMA_IRQ_0, true);
    const int positiveEdge = 1 << 3;
    uint32_t store_address = 0;
    uint8_t *vram_ptr = (uint8_t*)&vram;
    char digits[] = {'0','1','2','3','4','5','6','7','8','9','a','b','c','d','e','f'};
    while (true) {
//        if (!pio_sm_is_rx_fifo_empty(databus_pio, databus_sm)) {
            uint32_t v = store_address;
            for (int i = 0; i < 5; i++) {
                vram->screen[5-i] = digits[(v >> (4*i)) & 0xf];
            }
            uint32_t data = pio_sm_get_blocking(databus_pio, databus_sm);
            uint32_t addr = (data >> 8) & 3;
            data = data & 0xff;
            if (addr == 0) {
                vram_ptr[store_address] = data;
                store_address = (store_address + 1) & 0x1ffff;
            } else if (addr == 1) {
                store_address = (store_address & 0xFFFF) | ((data & 0x80) << 9);
                store_address = (store_address + (data & 0x7F)) & 0x1ffff;
            } else if (addr == 2) {
                store_address = (store_address & 0x1FF00) | data;
            } else if (addr == 3) {
                store_address = (store_address & 0x100FF) | (data << 8);
            }
        }
//    }
}
