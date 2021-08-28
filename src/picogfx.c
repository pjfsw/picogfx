//#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "vga.pio.h"

int main() {
    PIO pio = pio0;
    const uint8_t VGA_BASE_PIN = 8;

    uint offset = pio_add_program(pio, &vga_program);
    uint sm = pio_claim_unused_sm(pio, true);
    // freq = VGA frequency
    // float div = clock_get_hz(clk_sys) / freq;
    float div = 65535;
    vga_program_init(pio, sm, offset, VGA_BASE_PIN, div);

    uint8_t v = 0;
    while (true) {
        uint32_t v32 = (v++) | ((v++) << 8) | ((v++) << 16) | ((v++) << 24);
        pio_sm_put_blocking(pio, sm, v32);
    }
}
