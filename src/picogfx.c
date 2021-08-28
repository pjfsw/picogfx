//#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "vga.pio.h"

int main() {
    PIO pio = pio0;
    const uint8_t VGA_BASE_PIN = 8;

    uint offset = pio_add_program(pio, &vga_program);
    uint sm = pio_claim_unused_sm(pio, true);
    vga_program_init(pio, sm, offset, VGA_BASE_PIN);

    while (true) {
        pio_sm_put_blocking(pio, sm, 0xaa);
        sleep_ms(500);
        pio_sm_put_blocking(pio, sm, 0x55);
        sleep_ms(500);            
    }
}
