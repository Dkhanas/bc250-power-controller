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

# Firmware version. v9+: also the image version the RP2350 bootrom uses to pick between the two firmware slots
# (A/B, see partition_table.json), so raise it for every release: MAJOR = vN, MINOR = fix number (v9.1 -> 9, 1).
set(BC250_FW_MAJOR 9)
set(BC250_FW_MINOR 0)
if(BC250_FW_MINOR EQUAL 0)
    set(BC250_FW_VERSION "v${BC250_FW_MAJOR}")
else()
    set(BC250_FW_VERSION "v${BC250_FW_MAJOR}.${BC250_FW_MINOR}")
endif()
message(STATUS "BC-250 firmware ${BC250_FW_VERSION}")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/fw_version.txt "${BC250_FW_VERSION}")   # checked by the release workflow

# Shared by both firmware variants
set(BC250_SOURCES
    src/main.c
    src/bt_bridge.c
    src/usb_xinput.c
    src/usb_device.c
    src/pads.c
    src/log.c
    src/settings.c
    src/net.c
    src/dhcpd.c
    src/dnsd.c
    src/ota.c
    ${BTSTACK_MISSING_SOURCES}
)

# v8: web pages (firmware/web/) are embedded into the firmware as web_assets.h
set(WEB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/web)
set(WEB_FILES index.html setup.html settings.html style.css app.js manifest.webmanifest icon.png)
list(TRANSFORM WEB_FILES PREPEND ${WEB_DIR}/ OUTPUT_VARIABLE WEB_DEPS)
set(WEB_HDR ${CMAKE_CURRENT_BINARY_DIR}/generated/web_assets.h)
add_custom_command(OUTPUT ${WEB_HDR}
    COMMAND ${CMAKE_COMMAND} -DDIR=${WEB_DIR} -DOUT=${WEB_HDR} -P ${WEB_DIR}/embed.cmake
    DEPENDS ${WEB_DEPS} ${WEB_DIR}/embed.cmake
    COMMENT "Embedding web pages")
add_custom_target(bc250_web DEPENDS ${WEB_HDR})

# v9: flash layout with two firmware slots (A/B) for updates from the web page. The firmware carries it
# (generated/pt_data.h) and sets it up by itself the first time it starts on a Pico without it (ota.c), so only
# the firmware file is copied over USB. bc250_partition_table.uf2 is still built, as a fallback.
pico_init_picotool()
set(PT_JSON ${CMAKE_CURRENT_SOURCE_DIR}/partition_table.json)
set(PT_UF2 ${CMAKE_CURRENT_BINARY_DIR}/bc250_partition_table.uf2)
set(PT_HDR ${CMAKE_CURRENT_BINARY_DIR}/generated/pt_data.h)
add_custom_command(OUTPUT ${PT_UF2}
    COMMAND picotool partition create --quiet ${PT_JSON} ${PT_UF2}
    DEPENDS ${PT_JSON}
    COMMENT "Creating bc250_partition_table.uf2")
add_custom_command(OUTPUT ${PT_HDR}
    COMMAND ${CMAKE_COMMAND} -DUF2=${PT_UF2} -DJSON=${PT_JSON} -DOUT=${PT_HDR} -P ${CMAKE_CURRENT_SOURCE_DIR}/pt_embed.cmake
    DEPENDS ${PT_UF2} ${PT_JSON} ${CMAKE_CURRENT_SOURCE_DIR}/pt_embed.cmake
    COMMENT "Embedding the partition table")
add_custom_target(bc250_partition_table ALL DEPENDS ${PT_UF2} ${PT_HDR})
if(TARGET picotoolBuild)
    add_dependencies(bc250_partition_table picotoolBuild)   # picotool built from source first
endif()

# btstack_config.h / sdkconfig.h must be visible to libbluepad32 too
include_directories(src)
include_directories(${BTSTACK_ROOT}/3rd-party/bluedroid/encoder/include)
include_directories(${BTSTACK_ROOT}/3rd-party/bluedroid/decoder/include)
include_directories(${BTSTACK_ROOT}/src)

add_subdirectory(${BLUEPAD32_ROOT}/src/components/bluepad32 libbluepad32)

# v8: Bluepad32's CMake links pico_cyw43_arch_none (no Wi-Fi). Swap it for the lwIP variant so the whole
# firmware is built with the same CYW43 driver settings (CYW43_LWIP=1); Bluepad32 itself stays unmodified.
foreach(prop LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
    get_target_property(_bp32_libs bluepad32 ${prop})
    if(_bp32_libs)
        list(TRANSFORM _bp32_libs REPLACE "^pico_cyw43_arch_none$" "pico_cyw43_arch_lwip_threadsafe_background")
        set_target_properties(bluepad32 PROPERTIES ${prop} "${_bp32_libs}")
    endif()
endforeach()

# One firmware file per variant:
#   bc250_power.uf2     - everything, without Home Assistant
#   bc250_power_ha.uf2  - the same + Home Assistant (MQTT)
function(bc250_firmware name with_ha)
    add_executable(${name} ${BC250_SOURCES} ${WEB_HDR} ${PT_HDR})
    add_dependencies(${name} bc250_web bc250_partition_table)
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/generated
        src
        ${BLUEPAD32_ROOT}/src/components/bluepad32/include)
    target_link_libraries(${name} PUBLIC
        pico_stdlib
        pico_unique_id
        hardware_pwm
        hardware_watchdog
        hardware_flash
        pico_flash
        boot_uf2_headers
        pico_cyw43_arch_lwip_threadsafe_background
        pico_lwip_mdns
        pico_btstack_classic
        pico_btstack_cyw43
        tinyusb_device
        tinyusb_board
        bluepad32
    )
    if(with_ha)
        target_link_libraries(${name} PUBLIC pico_lwip_mqtt)
    endif()
    # Wi-Fi host name (DHCP); the web page is also announced as bc250.local (mDNS)
    target_compile_definitions(${name} PRIVATE CYW43_HOST_NAME=\"bc250\" BC250_HA=$<BOOL:${with_ha}>
        BC250_FW_VERSION=\"${BC250_FW_VERSION}\"
        PICO_CRT0_IMAGE_TYPE_TBYB=1)   # v9: an update starts "on trial" and is kept only once it runs (ota.c)
    # v9: version (A/B choice) + SHA-256 hash in the image: the bootrom refuses a damaged firmware
    pico_set_binary_version(${name} MAJOR ${BC250_FW_MAJOR} MINOR ${BC250_FW_MINOR})
    pico_hash_binary(${name})
    # USB belongs to the gamepad/power-key device, debug log goes to UART0 (GP0 TX)
    pico_enable_stdio_usb(${name} 0)
    pico_enable_stdio_uart(${name} 1)
    pico_add_extra_outputs(${name})
endfunction()

bc250_firmware(bc250_power OFF)
bc250_firmware(bc250_power_ha ON)

