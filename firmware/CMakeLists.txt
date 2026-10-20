cmake_minimum_required(VERSION 3.13)

# Raspberry Pi Pico 2 W (RP2350 + CYW43439)
if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD "pico2_w")
endif()

# Bluepad32 checkout (https://github.com/ricardoquesada/bluepad32)
if(NOT DEFINED BLUEPAD32_ROOT)
    set(BLUEPAD32_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../bluepad32)
endif()
get_filename_component(BLUEPAD32_ROOT ${BLUEPAD32_ROOT} ABSOLUTE)

# Bluepad32 needs the newer BTstack it ships with (external/btstack submodule)
set(BTSTACK_ROOT ${BLUEPAD32_ROOT}/external/btstack)
set(PICO_BTSTACK_PATH ${BTSTACK_ROOT})
set(BTSTACK_MISSING_SOURCES ${BTSTACK_ROOT}/src/hci_event_builder.c)

include(pico_sdk_import.cmake)

project(bc250_power C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()

add_executable(bc250_power
    src/main.c
    src/bt_bridge.c
    src/usb_xinput.c
    src/usb_device.c
    src/pads.c
    ${BTSTACK_MISSING_SOURCES}
)

target_include_directories(bc250_power PRIVATE
    src
    ${BLUEPAD32_ROOT}/src/components/bluepad32/include)

# btstack_config.h / sdkconfig.h must be visible to libbluepad32 too
include_directories(src)
include_directories(${BTSTACK_ROOT}/3rd-party/bluedroid/encoder/include)
include_directories(${BTSTACK_ROOT}/3rd-party/bluedroid/decoder/include)
include_directories(${BTSTACK_ROOT}/src)

target_link_libraries(bc250_power PUBLIC
    pico_stdlib
    pico_unique_id
    hardware_pwm
    hardware_watchdog
    pico_cyw43_arch_none
    pico_btstack_classic
    pico_btstack_cyw43
    tinyusb_device
    tinyusb_board
    bluepad32
)

add_subdirectory(${BLUEPAD32_ROOT}/src/components/bluepad32 libbluepad32)

# USB belongs to the gamepad/power-key device, debug log goes to UART0 (GP0 TX)
pico_enable_stdio_usb(bc250_power 0)
pico_enable_stdio_uart(bc250_power 1)

pico_add_extra_outputs(bc250_power)
