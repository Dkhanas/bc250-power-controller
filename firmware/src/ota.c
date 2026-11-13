// Firmware update from the web page, A/B slots with automatic rollback (see ota.h).
#include "ota.h"

#include <stdio.h>
#include <string.h>

#include "boot/picobin.h"
#include "boot/picoboot_constants.h"
#include "boot/uf2.h"
#include "config.h"
#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"
#include "hardware/structs/watchdog.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "pico/error.h"
#include "pico/flash.h"
#include "pico/stdlib.h"
#include "hardware/sync.h"
#include "pt_data.h"   // generated from partition_table.json (pt_embed.cmake)

#ifndef BC250_FW_VERSION
#define BC250_FW_VERSION "v?"
#endif

// Present in every firmware that can take web updates: an uploaded file without it is refused, so an update can
// never install a firmware that can't be updated the same way again.
static const char ota_marker[] = "BC250-POWER-WEB-UPDATE-1";

// watchdog scratch[3]: update state across the restart (main.c uses 0-2, the bootrom 4-7)
#define OTA_SCRATCH          3
#define OTA_PENDING_MAGIC    0x0DA70001u   // restarting into an update
#define OTA_TRIAL_MAGIC      0x0DA70002u   // the update is running, not kept yet
#define OTA_SLOTS_MAGIC      0x0DA70003u   // the two firmware slots were just set up (ota_setup_slots)

static int8_t boot_part = -1;    // partition (slot) we booted from: 0 = A, 1 = B, -1 = no partition table
static bool trial;               // bootrom: this firmware is on trial (buy pending)
static bool usb_install;         // ... but it was copied over USB, not sent from the page: keep it at once
static bool just_updated;
static char msg[120];            // result of the last update, shown on the page
static __attribute__((aligned(4))) uint8_t workarea[4096];

static char slot_name(int p) { return p == 0 ? 'A' : p == 1 ? 'B' : '-'; }

// Flash location of partition idx
static bool partition_range(int idx, uint32_t* off, uint32_t* size) {
    uint32_t buf[3];
    int rc = rom_get_partition_table_info(buf, 3, PT_INFO_PARTITION_LOCATION_AND_FLAGS | PT_INFO_SINGLE_PARTITION |
                                                      ((uint32_t)idx << 24));
    if (rc != 3) return false;
    uint32_t first = (buf[1] & PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_BITS) >> PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_LSB;
    uint32_t last = (buf[1] & PICOBIN_PARTITION_LOCATION_LAST_SECTOR_BITS) >> PICOBIN_PARTITION_LOCATION_LAST_SECTOR_LSB;
    *off = first * FLASH_SECTOR_SIZE;
    *size = (last + 1 - first) * FLASH_SECTOR_SIZE;
    return true;
}

// ---- flash writes (interrupts off while the flash is busy, as for the settings)
typedef struct {
    uint32_t off;
    const uint8_t* data;
    uint32_t len;
    bool erase;
} fop_t;

static void fop_fn(void* p) {
    fop_t* f = p;
    if (f->erase) flash_range_erase(f->off, f->len);
    else flash_range_program(f->off, f->data, f->len);
}

static bool flash_erase(uint32_t off, uint32_t len) {
    fop_t f = {off, NULL, len, true};
    return flash_safe_execute(fop_fn, &f, 1000) == PICO_OK;
}

static bool flash_prog(uint32_t off, const uint8_t* d, uint32_t len) {
    fop_t f = {off, d, len, false};
    return flash_safe_execute(fop_fn, &f, 1000) == PICO_OK;
}

// ================================================================== boot: trial, buy, rollback
void ota_boot_init(void) {
    boot_info_t bi;
    if (rom_get_boot_info(&bi)) {
        boot_part = bi.partition;
        trial = (bi.tbyb_and_update_info & BOOT_TBYB_AND_UPDATE_FLAG_BUY_PENDING) != 0;
    }
    if (boot_part > 1) boot_part = -1;   // not an A/B slot
    rom_load_partition_table(workarea, sizeof(workarea), false);   // after a watchdog start it isn't loaded yet
    uint32_t s = watchdog_hw->scratch[OTA_SCRATCH];
    if (!watchdog_caused_reboot()) s = 0;

    if (boot_part >= 0)
        printf("bc250: firmware %s in slot %c (web update ready)\n", BC250_FW_VERSION, slot_name(boot_part));
    else
        printf("bc250: firmware %s, old flash layout - web update needs one USB update first (README: Flash it)\n",
               BC250_FW_VERSION);

    if (trial && s != OTA_PENDING_MAGIC) {
        // Copied onto the Pico over USB (BOOTSEL). Keep it right away: people unplug the Pico soon after copying,
        // and a firmware still on trial would not start at the next power-up.
        usb_install = true;
        printf("bc250: firmware %s installed over USB\n", BC250_FW_VERSION);
    } else if (trial) {
        just_updated = true;
        printf("bc250: update: firmware %s started on trial - kept once Bluetooth runs (%lu s), otherwise the Pico "
               "goes back to the old firmware\n", BC250_FW_VERSION, (unsigned long)(OTA_BUY_AFTER_MS / 1000));
        snprintf(msg, sizeof(msg), "Starting %s…", BC250_FW_VERSION);
        watchdog_hw->scratch[OTA_SCRATCH] = OTA_TRIAL_MAGIC;
        return;
    }
    if (s == OTA_SLOTS_MAGIC && boot_part >= 0) {
        just_updated = true;
        printf("bc250: firmware slots set up - running from slot %c, updates from the page work now\n",
               slot_name(boot_part));
        snprintf(msg, sizeof(msg), "Firmware slots set up ✓ - updates from this page work now.");
    }
    if (s == OTA_PENDING_MAGIC || s == OTA_TRIAL_MAGIC) {
        just_updated = true;
        printf("bc250: update: the new firmware %s - back on %s (slot %c)\n",
               s == OTA_PENDING_MAGIC ? "did not start (rejected by the bootrom)" : "did not run properly",
               BC250_FW_VERSION, slot_name(boot_part));
        snprintf(msg, sizeof(msg), "The update didn't %s, so the Pico went back to %s.",
                 s == OTA_PENDING_MAGIC ? "start" : "run properly", BC250_FW_VERSION);
    }
    watchdog_hw->scratch[OTA_SCRATCH] = 0;
}

bool ota_just_updated(void) { return just_updated; }

// After a buy: make sure this firmware is also the one a normal start picks (the bootrom prefers the higher
// version number; reinstalling the same version must not leave the older copy in charge).
static void keep_this_slot(void) {
    if (boot_part < 0) return;
    int pick = rom_pick_ab_partition(workarea, sizeof(workarea), 0, 0);
    if (pick < 0 || pick == boot_part) return;
    uint32_t off, size;
    if (!partition_range(pick, &off, &size)) return;
    printf("bc250: update: slot %c has an equal or newer version number - clearing it so %s stays\n", slot_name(pick),
           BC250_FW_VERSION);
    flash_erase(off, FLASH_SECTOR_SIZE);
}

void ota_task(uint32_t t, bool bt_ready) {
    if (!trial) return;
    bool buy = boot_part < 0 /* no slots: nothing to go back to */ || usb_install || (t >= OTA_BUY_AFTER_MS && bt_ready);
    if (!buy) {
        if (t >= OTA_BUY_DEADLINE_MS) {
            printf("bc250: update: Bluetooth didn't start within %lu s - going back to the old firmware\n",
                   (unsigned long)(OTA_BUY_DEADLINE_MS / 1000));
            stdio_flush();
            watchdog_reboot(0, 0, 10);   // normal start: the bootrom never picks an image still on trial
            while (true) tight_loop_contents();
        }
        return;
    }
    int rc = rom_explicit_buy(workarea, sizeof(workarea));
    trial = false;
    watchdog_hw->scratch[OTA_SCRATCH] = 0;
    if (rc) {
        printf("bc250: update: keeping the firmware failed (%d)\n", rc);
        snprintf(msg, sizeof(msg), "Updated to %s, but keeping it failed (%d) - it may go back on the next restart.",
                 BC250_FW_VERSION, rc);
        return;
    }
    printf("bc250: update: firmware %s %s - kept (slot %c)\n", BC250_FW_VERSION,
           usb_install ? "installed over USB" : "runs fine", slot_name(boot_part));
    if (!usb_install) snprintf(msg, sizeof(msg), "Updated to %s ✓", BC250_FW_VERSION);
    keep_this_slot();
}

// ================================================================== upload
static struct {
    bool active;
    uint32_t part_off, part_size;   // target slot in flash
    uint32_t len, got;              // file size, bytes received
    uint8_t blk[512];
    uint32_t fill;
    uint32_t nblocks, done;         // UF2 blocks of the RP2350 (Arm) firmware
    bool wrote, marker;
    uint8_t erased[(2048 * 1024 / FLASH_SECTOR_SIZE) / 8];
    uint8_t tail[sizeof(ota_marker)];
    uint32_t tail_n;
    uint32_t last_pct;
} up;

static int up_fail(char* err, int en, const char* m) {
    snprintf(err, (size_t)en, "%s", m);
    snprintf(msg, sizeof(msg), "Update failed: %s", m);
    printf("bc250: update: failed - %s\n", m);
    ota_cancel();
    return -1;
}

bool ota_busy(void) { return up.active; }

void ota_cancel(void) {
    if (up.active && up.wrote) flash_erase(up.part_off, FLASH_SECTOR_SIZE);   // a partial image must never look bootable
    up.active = false;
}

int ota_begin(uint32_t len, char* err, int en) {
    if (up.active) return snprintf(err, (size_t)en, "another update is running"), -1;
    if (boot_part < 0)
        return snprintf(err, (size_t)en, "This Pico still has the old flash layout. Update it once over USB (README: "
                                         "Flash it), after that updates work from here."), -1;
    if (trial) return snprintf(err, (size_t)en, "the last update is still starting - try again in a minute"), -1;
    memset(&up, 0, sizeof(up));
    resident_partition_t part;
    rom_flash_flush_cache();
    if (rom_get_uf2_target_partition(workarea, sizeof(workarea), RP2350_ARM_S_FAMILY_ID, &part) < 0)
        return snprintf(err, (size_t)en, "no free firmware slot found"), -1;
    uint32_t first = (part.permissions_and_location & PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_BITS) >>
                     PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_LSB;
    uint32_t last = (part.permissions_and_location & PICOBIN_PARTITION_LOCATION_LAST_SECTOR_BITS) >>
                    PICOBIN_PARTITION_LOCATION_LAST_SECTOR_LSB;
    up.part_off = first * FLASH_SECTOR_SIZE;
    up.part_size = (last + 1 - first) * FLASH_SECTOR_SIZE;
    uint32_t own_off, own_size;
    if (partition_range(boot_part, &own_off, &own_size) && own_off == up.part_off)
        return snprintf(err, (size_t)en, "the free slot is the running one - refusing"), -1;
    if (up.part_size > sizeof(up.erased) * 8 * FLASH_SECTOR_SIZE)
        return snprintf(err, (size_t)en, "firmware slot larger than expected"), -1;
    if (!len || len % 512 || len / 2 > up.part_size + 4096)
        return snprintf(err, (size_t)en, "that's not a firmware file (.uf2) for this Pico"), -1;
    up.len = len;
    up.active = true;
    printf("bc250: update: receiving %lu KB into slot %c\n", (unsigned long)(len / 1024), slot_name(1 - boot_part));
    snprintf(msg, sizeof(msg), "Receiving the update…");
    return 0;
}

static void marker_scan(const uint8_t* d, uint32_t n) {
    if (up.marker) return;
    const uint32_t m = sizeof(ota_marker) - 1;
    uint8_t buf[sizeof(ota_marker) + 256];
    uint32_t k = up.tail_n;
    memcpy(buf, up.tail, k);
    memcpy(buf + k, d, n);
    k += n;
    for (uint32_t i = 0; i + m <= k; i++)
        if (buf[i] == ota_marker[0] && memcmp(buf + i, ota_marker, m) == 0) {
            up.marker = true;
            return;
        }
    up.tail_n = k < m - 1 ? k : m - 1;
    memcpy(up.tail, buf + k - up.tail_n, up.tail_n);
}

static int block(char* err, int en) {
    const struct uf2_block* b = (const struct uf2_block*)up.blk;
    if (b->magic_start0 != UF2_MAGIC_START0 || b->magic_start1 != UF2_MAGIC_START1 || b->magic_end != UF2_MAGIC_END)
        return up_fail(err, en, "that's not a .uf2 firmware file (or it's damaged)");
    // Only the RP2350 Arm firmware itself is written; the file may also hold other blocks (e.g. the
    // "absolute" block picotool adds for chip erratum E10).
    if ((b->flags & UF2_FLAG_NOT_MAIN_FLASH) || !(b->flags & UF2_FLAG_FAMILY_ID_PRESENT) ||
        b->file_size != RP2350_ARM_S_FAMILY_ID)
        return 0;
    if (!up.nblocks) up.nblocks = b->num_blocks;
    if (b->block_no != up.done || b->num_blocks != up.nblocks) return up_fail(err, en, "the file is damaged (block order)");
    if (b->payload_size != 256 || (b->target_addr & 255)) return up_fail(err, en, "unexpected .uf2 layout");
    if (b->target_addr < XIP_BASE || b->target_addr - XIP_BASE + 256 > up.part_size)
        return up_fail(err, en, "the firmware doesn't fit the slot");
    uint32_t rel = b->target_addr - XIP_BASE;
    uint32_t sec = rel / FLASH_SECTOR_SIZE;
    if (!(up.erased[sec / 8] & (1u << (sec % 8)))) {
        if (!flash_erase(up.part_off + sec * FLASH_SECTOR_SIZE, FLASH_SECTOR_SIZE))
            return up_fail(err, en, "flash erase failed");
        up.erased[sec / 8] |= (uint8_t)(1u << (sec % 8));
    }
    up.wrote = true;
    if (!flash_prog(up.part_off + rel, b->data, 256)) return up_fail(err, en, "flash write failed");
    marker_scan(b->data, 256);
    up.done++;
    return 0;
}

int ota_write(const uint8_t* d, uint32_t n, char* err, int en) {
    if (!up.active) return snprintf(err, (size_t)en, "no update running"), -1;
    if (up.got + n > up.len) return up_fail(err, en, "more data than announced");
    up.got += n;
    while (n) {
        uint32_t take = sizeof(up.blk) - up.fill;
        if (take > n) take = n;
        memcpy(up.blk + up.fill, d, take);
        up.fill += take;
        d += take;
        n -= take;
        if (up.fill == sizeof(up.blk)) {
            up.fill = 0;
            if (block(err, en)) return -1;
        }
    }
    uint32_t pct = (uint32_t)((uint64_t)up.got * 100 / up.len);
    if (pct / 25 != up.last_pct / 25) printf("bc250: update: %lu %%\n", (unsigned long)pct);
    up.last_pct = pct;
    return 0;
}

int ota_end(char* m, int mn) {
    if (!up.active) return snprintf(m, (size_t)mn, "no update running"), -1;
    if (up.got != up.len || up.fill) return up_fail(m, mn, "the file arrived incomplete");
    if (!up.nblocks) return up_fail(m, mn, "this .uf2 isn't for the Pico 2 W (RP2350, Arm)");
    if (up.done != up.nblocks) return up_fail(m, mn, "the file is incomplete");
    if (!up.marker)
        return up_fail(m, mn, "this isn't BC-250 power controller firmware with web update (v9 or newer)");
    up.active = false;
    printf("bc250: update: %lu blocks written to slot %c - restarting into it\n", (unsigned long)up.done,
           slot_name(1 - boot_part));
    snprintf(m, (size_t)mn, "Firmware received. The Pico restarts into it now (about 20 s).");
    snprintf(msg, sizeof(msg), "Restarting into the update…");
    return 0;
}

void ota_reboot(void) {
    watchdog_hw->scratch[OTA_SCRATCH] = OTA_PENDING_MAGIC;
    stdio_flush();
    rom_reboot(REBOOT2_FLAG_REBOOT_TYPE_FLASH_UPDATE, 100, XIP_BASE + up.part_off, 0);
    while (true) tight_loop_contents();
}

int ota_status_json(char* b, int n) {
    // msg is ours (no quotes/backslashes in it)
    return snprintf(b, (size_t)n, "\"fw\":{\"version\":\"%s\",\"slot\":\"%c\",\"web_update\":%s,\"trial\":%s,\"busy\":%s,"
                                  "\"msg\":\"%s\"}",
                    BC250_FW_VERSION, slot_name(boot_part), boot_part >= 0 ? "true" : "false", trial ? "true" : "false",
                    up.active ? "true" : "false", msg);
}

// ================================================================== one-time: set up the two slots (v9)
// A firmware copied over USB onto a Pico without slots lands at the start of flash (like v8). On its first start it
// copies itself into slot B, checks the copy, then writes the partition table over the start of flash and restarts:
// the Pico then runs from slot B, with A free for the next update. Settings / pairing keys (end of flash) stay.
// Only the last step (about 50 ms) can't be undone; power lost during it = copy the firmware over USB again.
extern char __flash_binary_end;
static uint8_t copy_buf[FLASH_SECTOR_SIZE];
static uint8_t pt_ram[PT_BLOCKS][256];
static uint32_t pt_off_ram[PT_BLOCKS];
static rom_reboot_fn reboot_rom;

// Runs from RAM with interrupts off: the start of flash (where this firmware's own code also is) is replaced,
// so nothing may run from flash any more. Ends with a restart.
static void __no_inline_not_in_flash_func(write_pt_and_restart)(void) {
    save_and_disable_interrupts();
    flash_range_erase(0, FLASH_SECTOR_SIZE);
    for (int i = 0; i < PT_BLOCKS; i++) flash_range_program(pt_off_ram[i], pt_ram[i], 256);
    reboot_rom(REBOOT2_FLAG_REBOOT_TYPE_NORMAL, 10, 0, 0);
    while (true) __asm volatile("wfi");
}

static void slots_skip(const char* why) {
    printf("bc250: slots: not set up - %s\n", why);
    snprintf(msg, sizeof(msg), "Firmware slots not set up: %s.", why);
}

void ota_setup_slots(void) {
    if (boot_part >= 0) return;   // already running from a slot
    // Running from the very start of flash? Then the start of flash (no address translation) holds exactly what this
    // firmware sees at its own start. Running from a slot, the start of flash holds the partition table instead.
    if (memcmp((const void*)XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE, (const void*)XIP_NOCACHE_NOALLOC_BASE,
               FLASH_SECTOR_SIZE) != 0) {
        slots_skip("not running from the start of flash");
        return;
    }
    uint32_t info[3];
    int rc = rom_get_partition_table_info(info, 3, PT_INFO_PT_INFO);
    printf("bc250: slots: none yet (boot %d, partition table info %d / 0x%lx)\n", boot_part, rc,
           rc >= 2 ? (unsigned long)info[1] : 0ul);
    // (no further check on that info: this firmware's own start is at the start of flash, so no partition table
    //  can be there)
    uint32_t size = (uint32_t)((uintptr_t)&__flash_binary_end - XIP_BASE) + 8192;   // + end block / margin
    size = (size + FLASH_SECTOR_SIZE - 1) & ~(FLASH_SECTOR_SIZE - 1);
    if (size > SLOT_B_SIZE || size > SLOT_B_OFFSET) {
        slots_skip("firmware too large for a slot");
        return;
    }
    if (trial) {   // keep this firmware first (the copy must be the kept version)
        if (rom_explicit_buy(workarea, sizeof(workarea))) {
            slots_skip("keeping the firmware failed");
            return;
        }
        trial = false;
    }
    printf("bc250: setting up the two firmware slots (one time, a few seconds) - copying %lu KB into slot B\n",
           (unsigned long)(size / 1024));
    const uint8_t* src = (const uint8_t*)XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE;
    const uint8_t* dst = src + SLOT_B_OFFSET;
    uint32_t erase = (size + 0xFFFFu) & ~0xFFFFu;   // whole 64 KB blocks (fast); slot B is 64 KB aligned
    if (erase > SLOT_B_SIZE) erase = size;
    if (!flash_erase(SLOT_B_OFFSET, erase)) {
        slots_skip("flash erase failed");
        return;
    }
    for (uint32_t off = 0; off < size; off += FLASH_SECTOR_SIZE) {
        memcpy(copy_buf, src + off, FLASH_SECTOR_SIZE);
        if (!flash_prog(SLOT_B_OFFSET + off, copy_buf, FLASH_SECTOR_SIZE)) {
            slots_skip("flash write failed");
            flash_erase(SLOT_B_OFFSET, FLASH_SECTOR_SIZE);
            return;
        }
        watchdog_update();
    }
    if (memcmp(dst, src, size) != 0) {
        slots_skip("the copy didn't match");
        flash_erase(SLOT_B_OFFSET, FLASH_SECTOR_SIZE);
        return;
    }
    printf("bc250: slots: copy checked - writing the partition table and restarting\n");
    for (int i = 0; i < PT_BLOCKS; i++) {
        memcpy(pt_ram[i], pt_data[i], 256);
        pt_off_ram[i] = pt_offset[i];
    }
    reboot_rom = (rom_reboot_fn)rom_func_lookup(ROM_FUNC_REBOOT);
    watchdog_hw->scratch[OTA_SCRATCH] = OTA_SLOTS_MAGIC;
    stdio_flush();
    sleep_ms(20);   // let the UART finish
    write_pt_and_restart();
}
