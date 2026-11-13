# v9: turns bc250_partition_table.uf2 into a C header, so the firmware can set up its two slots by itself when it
# was copied onto a Pico without them (see ota_setup_slots() in src/ota.c).
# Run by the build: cmake -DUF2=<partition table uf2> -DJSON=<partition_table.json> -DOUT=<header> -P pt_embed.cmake
file(READ ${UF2} hex HEX)
string(LENGTH "${hex}" n)
math(EXPR nblocks "${n} / 1024")
math(EXPR whole "${nblocks} * 1024")
if(nblocks LESS 1 OR NOT n EQUAL whole)
    message(FATAL_ERROR "${UF2}: not a UF2 file")
endif()
# little-endian 32-bit word at byte offset off of block b
function(le32 b off outvar)
    math(EXPR p "(${b} * 512 + ${off}) * 2")
    string(SUBSTRING "${hex}" ${p} 8 w)
    string(SUBSTRING "${w}" 6 2 b3)
    string(SUBSTRING "${w}" 4 2 b2)
    string(SUBSTRING "${w}" 2 2 b1)
    string(SUBSTRING "${w}" 0 2 b0)
    math(EXPR v "0x${b3}${b2}${b1}${b0}" OUTPUT_FORMAT HEXADECIMAL)
    set(${outvar} ${v} PARENT_SCOPE)
endfunction()
set(blocks "")
set(addrs "")
math(EXPR last "${nblocks} - 1")
foreach(b RANGE ${last})
    le32(${b} 0 m0)
    le32(${b} 12 addr)
    le32(${b} 16 size)
    if(NOT m0 EQUAL 0x0A324655 OR NOT size EQUAL 256)
        message(FATAL_ERROR "${UF2}: unexpected UF2 block ${b}")
    endif()
    if(addr LESS 0x10000000 OR addr GREATER 0x10000F00)
        message(FATAL_ERROR "${UF2}: partition table must lie in the first 4 KB of flash (block at ${addr})")
    endif()
    math(EXPR p "(${b} * 512 + 32) * 2")
    string(SUBSTRING "${hex}" ${p} 512 data)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," data "${data}")
    string(APPEND blocks "    {${data}},\n")
    math(EXPR off "${addr} - 0x10000000")
    string(APPEND addrs "${off}u, ")
endforeach()
# slot B (second partition) of partition_table.json: where the firmware copies itself
file(READ ${JSON} js)
string(JSON bstart GET "${js}" partitions 1 start)
string(JSON bsize GET "${js}" partitions 1 size)
foreach(v bstart bsize)
    if(NOT ${v} MATCHES "^([0-9]+)K$")
        message(FATAL_ERROR "partition_table.json: slot B start/size must be given in K")
    endif()
    math(EXPR ${v} "${CMAKE_MATCH_1} * 1024")
endforeach()
file(WRITE ${OUT} "// Generated from bc250_partition_table.uf2 by pt_embed.cmake - do not edit.
#pragma once
#include <stdint.h>
#define PT_BLOCKS ${nblocks}
static const uint32_t pt_offset[PT_BLOCKS] = {${addrs}};
static const uint8_t pt_data[PT_BLOCKS][256] = {
${blocks}};
#define SLOT_B_OFFSET ${bstart}u
#define SLOT_B_SIZE ${bsize}u
")
