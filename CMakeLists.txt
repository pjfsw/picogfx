cmake_minimum_required(VERSION 3.13)

# initialize the SDK based on PICO_SDK_PATH
# note: this must happen before project()
include(pico_sdk_import.cmake)

project(picogfx)

# initialize the Raspberry Pi Pico SDK
pico_sdk_init()

add_executable(picogfx)
add_executable(picogfx2)

pico_generate_pio_header(picogfx ${CMAKE_CURRENT_LIST_DIR}/src/vga.pio)
pico_generate_pio_header(picogfx ${CMAKE_CURRENT_LIST_DIR}/src/databus.pio)

pico_generate_pio_header(picogfx2 ${CMAKE_CURRENT_LIST_DIR}/src/vga.pio)

target_sources(picogfx PRIVATE
    src/picogfx.c
    )

target_sources(picogfx2 PRIVATE
    src/picogfx2.c
    src/picogfx2_output.c
    )

target_link_libraries(picogfx PRIVATE
    pico_stdlib
    hardware_pio
    hardware_irq
    hardware_dma
    )

target_link_libraries(picogfx2 PRIVATE
    pico_stdlib
    hardware_pio
    hardware_irq
    hardware_dma
    pico_multicore
    )

pico_add_extra_outputs(picogfx2)
