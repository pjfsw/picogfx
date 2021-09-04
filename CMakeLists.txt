cmake_minimum_required(VERSION 3.13)

# initialize the SDK based on PICO_SDK_PATH
# note: this must happen before project()
include(pico_sdk_import.cmake)

project(picogfx)

# initialize the Raspberry Pi Pico SDK
pico_sdk_init()

add_executable(picogfx)

pico_generate_pio_header(picogfx ${CMAKE_CURRENT_LIST_DIR}/src/vga.pio)
pico_generate_pio_header(picogfx ${CMAKE_CURRENT_LIST_DIR}/src/databus.pio)

target_sources(picogfx PRIVATE
    src/picogfx.c
    )

target_link_libraries(picogfx PRIVATE
    pico_stdlib
    hardware_pio
    hardware_irq
    hardware_dma
    )
pico_enable_stdio_usb(picogfx 1)
pico_enable_stdio_uart(picogfx 0)

pico_add_extra_outputs(picogfx)
