//#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/regs/sio.h"
/*
**
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "pico/stdlib.h"

int main() {
/*    uint32_t *outputs = (uint32_t*)(SIO_BASE + SIO_GPIO_OE_SET_OFFSET);
    *outputs = 0x0000ff00; // PIO8-15 = vga out    
    uint8_t *vga = (uint8_t*)(SIO_BASE + SIO_GPIO_OUT_OFFSET + 1);
    uint8_t rgbhv = 0;
    while (true) {
        *vga = rgbhv;
        rgbhv++;
        sleep_ms(250);
    }*/
    const uint LED_BASE = 8;
    for (int i = 0; i < 8; i++) {
      gpio_init(LED_BASE + i);
      gpio_set_dir(LED_BASE + i, GPIO_OUT);
    }
    uint8_t rgbhv = 0;
    while (true) {
      for (int i = 0; i < 8; i++) {
        if ((rgbhv & (1<<i)) > 0) {
          gpio_put(LED_BASE + i, 1);
        } else {
          gpio_put(LED_BASE + i, 0);
        }        
      }
      rgbhv++;
      sleep_ms(100);          
    }


}

