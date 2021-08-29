PicoGFX project
===============

The goal is to create a VGA-output graphics card using a Raspberry Pi Pico, which is addressable from a microprocessor such as 65C02. It should at least have background graphics but ideally sprites and characters/tiles.
It will use a 6-bit DAC to save pins and also to allowing for a complete VGA-signal with HSYNC and VSYNC to be transferred in one byte.

Future goals is to add bootstrap code for the 65C02.
