# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.1)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.1)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico CACHE STRING "Board type")

cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(rp2040_usb_bulk_test C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(rp2040_usb_bulk_test
    main.c
)

target_link_libraries(rp2040_usb_bulk_test PRIVATE
    pico_stdlib
    hardware_irq
    hardware_resets
)

# This project owns the native USB controller directly.
pico_enable_stdio_usb(rp2040_usb_bulk_test 0)
pico_enable_stdio_uart(rp2040_usb_bulk_test 0)

pico_add_extra_outputs(rp2040_usb_bulk_test)
