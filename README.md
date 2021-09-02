PicoGFX project
===============

The goal is to create a VGA-output graphics card using a Raspberry Pi Pico, which is addressable from a microprocessor such as 65C02. It should at least have background graphics but ideally sprites and characters/tiles.
It will use a 6-bit DAC to save pins and also to allowing for a complete VGA-signal with HSYNC and VSYNC to be transferred in one byte.

Future goals is to add bootstrap code for the 65C02.

Pinmapping:

    PIO00   Blue 0
    PIO01   Blue 1
    PIO02   Green 0
    PIO03   Green 1
    PIO04   Red 0
    PIO05   Red 1
    PIO06   HSync
    PIO07   VSync

    PIO08   D0
    PIO09   D1
    PIO10   D2
    PIO11   D3
    PIO12   D4
    PIO14   D5
    PIO15   D6
    PIO16   D7

    PIO17   A0
    PIO18   A1
    PIO19   /Chip Select
