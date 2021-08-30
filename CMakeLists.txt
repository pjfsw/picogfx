cmake_minimum_required(VERSION 3.13)

# initialize the SDK based on PICO_SDK_PATH
# note: this must happen before project()
include(pico_sdk_import.cmake)

project(picogfx)

# initialize the Raspberry Pi Pico SDK
pico_sdk_init()

add_executable(picogfx)

pico_generate_pio_header(picogfx ${CMAKE_CURRENT_LIST_DIR}/src/vga.pio)

target_sources(picogfx PRIVATE
    src/picogfx.c
    src/core2.c
    )

target_link_libraries(picogfx PRIVATE
    pico_stdlib
    pico_multicore
    hardware_pio
    hardware_irq
    hardware_dma
    )
pico_add_extra_outputs(picogfx)
