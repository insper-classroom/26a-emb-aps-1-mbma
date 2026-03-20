add_executable(pico_emb main.c)

set_target_properties(pico_emb PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# pull in common dependencies
target_link_libraries(pico_emb
    pico_stdlib
    hardware_pwm
    hardware_irq
    hardware_clocks
    hardware_gpio
    pico_multicore
)

# create map/bin/hex/uf2 file etc.
pico_add_extra_outputs(pico_emb)
pico_enable_stdio_usb(pico_emb 1)
pico_enable_stdio_uart(pico_emb 0)
