#include "bt_bridge.h"

#include <stdio.h>
#include <string.h>
#include <btstack.h>
#include "btstack_tlv.h"
#include <uni.h>

#include "config.h"
#include "pico/cyw43_arch.h"
#include "pico/time.h"

#include "sdkconfig.h"

#ifndef CONFIG_BLUEPAD32_PLATFORM_CUSTOM
#error "Pico W must use BLUEPAD32_PLATFORM_CUSTOM"
#endif
#if CONFIG_BLUEPAD32_MAX_DEVICES < MAX_PADS
#error "CONFIG_BLUEPAD32_MAX_DEVICES (sdkconfig.h) must be >= MAX_PADS (config.h)"
#endif

volatile uint32_t bt_ev_connected, bt_ev_system_button, bt_ev_activity;

// Per slot Bluetooth-side state
typedef struct {
    uni_hid_device_t* dev;
    uint32_t prev_buttons;
    report_parse_input_report_fn_t orig_input;
    report_parse_feature_report_fn_t orig_feature;
    // feature-report fetching
    const uint8_t* fetch_list;
    uint8_t fetch_n, fetch_i, fetch_tries;
    bool fetch_sent;
    uint32_t fetch_sent_at;
    uint8_t ds5_seq;
} bt_slot_t;

static bt_slot_t bs[MAX_PADS];

static volatile uint32_t h_ready_events;     // health check: a controller became ready
static void bt_note_ready(void) { h_ready_events++; }
static void kb_note_ready(bd_addr_t addr);

static const uint8_t fetch_ds5[] = {0x09, 0x20, 0x05};   // pairing (MAC), firmware, calibration
static const uint8_t fetch_ds4[] = {0x05, 0xA3};         // calibration (BT layout), firmware

static int slot_of(const uni_hid_device_t* d) {
    for (int i = 0; i < MAX_PADS; i++)
        if (bs[i].dev == d) return i;
    return -1;
}

static void xi_neutral(xinput_report_t* r) {
    memset(r, 0, sizeof(*r));
    r->length = 0x14;
}

// ------------------------------------------------------------------ Xbox 360 conversion

static int16_t axis(int32_t v, bool invert) {   // Bluepad32 -512..511 -> -32768..32767
    if (invert) v = -v - 1;
    v *= 64;
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    return (int16_t)v;
}

static uint8_t trigger(int32_t analog, bool digital) {   // 0..1023 -> 0..255
    int32_t v = analog >> 2;
    if (v <= 0 && digital) v = 255;
    if (v > 255) v = 255;
    if (v < 0) v = 0;
    return (uint8_t)v;
}

static void to_xinput(const uni_gamepad_t* gp, xinput_report_t* r) {
    xi_neutral(r);
    uint16_t b = 0;
    if (gp->dpad & DPAD_UP) b |= XI_DPAD_UP;
    if (gp->dpad & DPAD_DOWN) b |= XI_DPAD_DOWN;
    if (gp->dpad & DPAD_LEFT) b |= XI_DPAD_LEFT;
    if (gp->dpad & DPAD_RIGHT) b |= XI_DPAD_RIGHT;
    if (gp->misc_buttons & MISC_BUTTON_START) b |= XI_START;
    if (gp->misc_buttons & MISC_BUTTON_SELECT) b |= XI_BACK;
    if (gp->misc_buttons & MISC_BUTTON_SYSTEM) b |= XI_GUIDE;
    if (gp->buttons & BUTTON_THUMB_L) b |= XI_LS;
    if (gp->buttons & BUTTON_THUMB_R) b |= XI_RS;
    if (gp->buttons & BUTTON_SHOULDER_L) b |= XI_LB;
    if (gp->buttons & BUTTON_SHOULDER_R) b |= XI_RB;
    if (gp->buttons & BUTTON_A) b |= XI_A;
    if (gp->buttons & BUTTON_B) b |= XI_B;
    if (gp->buttons & BUTTON_X) b |= XI_X;
    if (gp->buttons & BUTTON_Y) b |= XI_Y;
    r->buttons = b;
    r->lt = trigger(gp->brake, gp->buttons & BUTTON_TRIGGER_L);
    r->rt = trigger(gp->throttle, gp->buttons & BUTTON_TRIGGER_R);
    r->lx = axis(gp->axis_x, false);
    r->ly = axis(gp->axis_y, true);    // Bluepad32: down = +, XInput: up = +
    r->rx = axis(gp->axis_rx, false);
    r->ry = axis(gp->axis_ry, true);
}

// ------------------------------------------------------------------ native passthrough taps

static pkt_t* feature_slot(pad_t* p, uint8_t id) {
    if (p->kind == NAT_DS5) {
        if (id == 0x05) return &p->feat_calib;
        if (id == 0x09) return &p->feat_pairing;
        if (id == 0x20) return &p->feat_firmware;
    } else if (p->kind == NAT_DS4) {
        if (id == 0x05) return &p->feat_calib;
        if (id == 0xA3) return &p->feat_firmware;
    }
    return NULL;
}

// Every raw input report: let Bluepad32 parse it, and keep a USB-layout copy
static void wrap_input(uni_hid_device_t* d, const uint8_t* r, uint16_t len) {
    int i = slot_of(d);
    if (i < 0) return;
    if (bs[i].orig_input) bs[i].orig_input(d, r, len);
    pad_t* p = &pads[i];
    if (!p->used || p->fetching || len < 1) return;

    uint8_t u[64] = {0};
    switch (p->kind) {
        case NAT_DS5:   // BT 0x31: id, seq, <USB report body>
            if (r[0] != 0x31 || len < 2 + 63) return;
            u[0] = 0x01;
            memcpy(&u[1], &r[2], 63);
            break;
        case NAT_DS4:   // BT 0x11: id, 2 header bytes, <USB report body>
            if (r[0] != 0x11 || len < 3 + 63) return;
            u[0] = 0x01;
            memcpy(&u[1], &r[3], 63);
            break;
        case NAT_SWITCH:   // same report over BT and USB; USB pads it to 64
            if (r[0] == 0x3F) return;   // simple-HID mode, not used over USB
            memcpy(u, r, len > 64 ? 64 : len);
            break;
        default:
            return;
    }
    ring_push(&p->to_usb, u, 64);
}

static void wrap_feature(uni_hid_device_t* d, const uint8_t* r, uint16_t len) {
    int i = slot_of(d);
    if (i < 0) return;
    bt_slot_t* s = &bs[i];
    pad_t* p = &pads[i];
    if (p->fetching && s->fetch_sent && len >= 1 && r[0] == s->fetch_list[s->fetch_i]) {
        pkt_t* f = feature_slot(p, r[0]);
        if (f) {
            f->len = (uint8_t)(len > 64 ? 64 : len);
            memcpy(f->data, r, f->len);
        }
        s->fetch_i++;
        s->fetch_sent = false;
        s->fetch_tries = 0;
        return;   // our request - don't feed a repeated answer to the Bluepad32 parser
    }
    if (s->orig_feature) s->orig_feature(d, r, len);
}

static uint32_t crc_ps(const uint8_t* buf, uint16_t len) {   // buf starts with the 0xA2 header
    return ~uni_crc32_le(0xffffffff, buf, len);
}

static void send_native_output(int i, const pkt_t* pk) {
    uni_hid_device_t* d = bs[i].dev;
    uint8_t b[79] = {0};
    switch (pads[i].kind) {
        case NAT_DS5: {   // USB 02 <47 common> -> BT A2 31 <seq> 10 <47 common> ... crc
            b[0] = 0xA2;
            b[1] = 0x31;
            b[2] = (uint8_t)(bs[i].ds5_seq << 4);
            bs[i].ds5_seq = (uint8_t)((bs[i].ds5_seq + 1) & 0x0F);
            b[3] = 0x10;
            memcpy(&b[4], &pk->data[1], 47);
            uint32_t crc = crc_ps(b, 75);
            memcpy(&b[75], &crc, 4);
            uni_hid_device_send_intr_report(d, b, 79);
            break;
        }
        case NAT_DS4: {   // USB 05 <common> -> BT A2 11 C4 00 <common> ... crc
            b[0] = 0xA2;
            b[1] = 0x11;
            b[2] = 0xC4;
            b[3] = 0x00;
            memcpy(&b[4], &pk->data[1], 31);
            uint32_t crc = crc_ps(b, 75);
            memcpy(&b[75], &crc, 4);
            uni_hid_device_send_intr_report(d, b, 79);
            break;
        }
        case NAT_SWITCH: {   // same report, 49 bytes over BT
            b[0] = 0xA2;
            memcpy(&b[1], pk->data, pk->len > 49 ? 49 : pk->len);
            uni_hid_device_send_intr_report(d, b, 50);
            break;
        }
        default:
            break;
    }
}

void bt_native_task(uint32_t t) {
    for (int i = 0; i < MAX_PADS; i++) {
        bt_slot_t* s = &bs[i];
        pad_t* p = &pads[i];
        if (!p->used || !s->dev) continue;

        if (p->fetching) {
            if (s->fetch_i >= s->fetch_n) {
                p->fetching = false;
                logi("bc250: slot %d ready for passthrough (kind %d)\n", i, p->kind);
            } else if (!s->fetch_sent || t - s->fetch_sent_at > 400) {
                if (s->fetch_sent && ++s->fetch_tries >= 3) {   // no answer: skip this report
                    s->fetch_i++;
                    s->fetch_tries = 0;
                    s->fetch_sent = false;
                    continue;
                }
                uint8_t req[2] = {(HID_MESSAGE_TYPE_GET_REPORT << 4) | HID_REPORT_TYPE_FEATURE, s->fetch_list[s->fetch_i]};
                uni_hid_device_send_ctrl_report(s->dev, req, sizeof(req));
                s->fetch_sent = true;
                s->fetch_sent_at = t;
            }
            continue;
        }

        pkt_t pk;
        while (ring_pop(&p->to_bt, &pk)) send_native_output(i, &pk);
    }
}

// ------------------------------------------------------------------ platform callbacks

static void p_init(int argc, const char** argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
}

static void p_on_init_complete(void) {
    // Paired controllers are kept in flash; they reconnect when you press their PS/Xbox/Home button.
    // Scanning stays on so a controller put in pairing mode gets paired automatically.
    uni_bt_list_keys_unsafe();
    uni_bt_start_scanning_and_autoconnect_unsafe();
}

static uni_error_t p_on_device_discovered(bd_addr_t addr, const char* name, uint16_t cod, uint8_t rssi) {
    (void)addr;
    ARG_UNUSED(name);
    ARG_UNUSED(rssi);
    uint16_t minor = cod & UNI_BT_COD_MINOR_MASK;
    if ((minor & UNI_BT_COD_MINOR_KEYBOARD) == UNI_BT_COD_MINOR_KEYBOARD ||
        (minor & UNI_BT_COD_MINOR_MICE) == UNI_BT_COD_MINOR_MICE)
        return UNI_ERROR_IGNORE_DEVICE;   // gamepads only
    return UNI_ERROR_SUCCESS;
}

static void p_on_device_connected(uni_hid_device_t* d) { ARG_UNUSED(d); }

static void p_on_device_disconnected_slot(int i) {
    pads[i].used = false;
    pads[i].fetching = false;
    ring_clear(&pads[i].to_usb);
    memset(&bs[i], 0, sizeof(bs[i]));
    logi("bc250: controller in slot %d disconnected\n", i);
}

static void p_on_device_disconnected(uni_hid_device_t* d) {
    int i = slot_of(d);
    if (i < 0) return;
    p_on_device_disconnected_slot(i);
}

static uni_error_t p_on_device_ready(uni_hid_device_t* d) {
    int i = slot_of(NULL);
    if (i < 0) return UNI_ERROR_NO_SLOTS;
    bt_slot_t* s = &bs[i];
    pad_t* p = &pads[i];
    memset(s, 0, sizeof(*s));
    s->dev = d;

    p->kind = NAT_NONE;
    if (d->controller_type == CONTROLLER_TYPE_PS5Controller) p->kind = NAT_DS5;
    else if (d->controller_type == CONTROLLER_TYPE_PS4Controller) p->kind = NAT_DS4;
    else if (d->controller_type == CONTROLLER_TYPE_SwitchProController) p->kind = NAT_SWITCH;
    p->pid = d->product_id;
    uni_bt_conn_get_address(&d->conn, p->mac);
    memset(&p->feat_calib, 0, sizeof(pkt_t));
    memset(&p->feat_pairing, 0, sizeof(pkt_t));
    memset(&p->feat_firmware, 0, sizeof(pkt_t));
    xi_neutral(&p->xi);
    p->xi_dirty = true;
    ring_clear(&p->to_usb);

    if (p->kind == NAT_DS5) {
        s->fetch_list = fetch_ds5;
        s->fetch_n = sizeof(fetch_ds5);
    } else if (p->kind == NAT_DS4) {
        s->fetch_list = fetch_ds4;
        s->fetch_n = sizeof(fetch_ds4);
    }
    p->fetching = s->fetch_n > 0;

    // Tap the raw report stream (the Bluepad32 parsers still get everything)
    s->orig_input = d->report_parser.parse_input_report;
    s->orig_feature = d->report_parser.parse_feature_report;
    if (s->orig_input) d->report_parser.parse_input_report = wrap_input;
    d->report_parser.parse_feature_report = wrap_feature;

    p->used = true;
    bt_ev_connected++;
    bt_note_ready();
    kb_note_ready(d->conn.btaddr);   // v6: back up this controller's pairing key
    logi("bc250: controller ready in slot %d (kind %d, pid %04x)\n", i, p->kind, p->pid);
    return UNI_ERROR_SUCCESS;
}

static void p_on_controller_data(uni_hid_device_t* d, uni_controller_t* ctl) {
    if (ctl->klass != UNI_CONTROLLER_CLASS_GAMEPAD) return;
    int i = slot_of(d);
    if (i < 0) return;
    const uni_gamepad_t* gp = &ctl->gamepad;

    uint32_t now_buttons = ((uint32_t)gp->buttons) | ((uint32_t)gp->misc_buttons << 16) | ((uint32_t)gp->dpad << 24);
    if (now_buttons & ~bs[i].prev_buttons) bt_ev_activity++;   // a new press
    bs[i].prev_buttons = now_buttons;

    xinput_report_t r;
    to_xinput(gp, &r);
    if (memcmp(&r, &pads[i].xi, sizeof(r)) != 0) {
        pads[i].xi = r;
        pads[i].xi_dirty = true;
    }
}

static const uni_property_t* p_get_property(uni_property_idx_t idx) {
    ARG_UNUSED(idx);
    return NULL;
}

static void p_on_oob_event(uni_platform_oob_event_t event, void* data) {
    ARG_UNUSED(data);
    if (event == UNI_PLATFORM_OOB_GAMEPAD_SYSTEM_BUTTON) bt_ev_system_button++;
}

static struct uni_platform platform = {
    .name = "BC-250 power controller",
    .init = p_init,
    .on_init_complete = p_on_init_complete,
    .on_device_discovered = p_on_device_discovered,
    .on_device_connected = p_on_device_connected,
    .on_device_disconnected = p_on_device_disconnected,
    .on_device_ready = p_on_device_ready,
    .on_oob_event = p_on_oob_event,
    .on_controller_data = p_on_controller_data,
    .get_property = p_get_property,
};

// ------------------------------------------------------------------ pairing key guard (v6)
// Bluepad32 deletes a controller's stored link key whenever opening its L2CAP channel fails once
// (uni_bt_bredr.c, on_l2cap_channel_opened). After that the controller keeps reconnecting with its
// key, the Pico has none, and only re-pairing helps. We keep our own copy of every key that worked
// (in the same flash TLV store, under our own tags) and put it back as soon as it disappears.

#define KEYBK_MAX 8
#define KEYBK_TAG(i) (0x42434B00u | (uint32_t)(i))   // 'B' 'C' 'K' + index

typedef struct {
    bd_addr_t addr;
    link_key_t key;
    uint8_t type;
    uint8_t valid;
} keybk_t;

static keybk_t kb[KEYBK_MAX];
static bool kb_loaded;
static uint32_t kb_checked_at, kb_restores;
static uint8_t kb_next;   // round-robin slot for a new controller when all are used

static bool kb_tlv(const btstack_tlv_t** impl, void** ctx) {
    *impl = NULL;
    *ctx = NULL;
    btstack_tlv_get_instance(impl, ctx);
    return *impl != NULL;
}

static void kb_load(void) {
    const btstack_tlv_t* impl;
    void* ctx;
    if (!kb_tlv(&impl, &ctx)) return;
    int n = 0;
    for (int i = 0; i < KEYBK_MAX; i++) {
        keybk_t e;
        if (impl->get_tag(ctx, KEYBK_TAG(i), (uint8_t*)&e, sizeof(e)) == (int)sizeof(e) && e.valid == 1) {
            kb[i] = e;
            n++;
        } else {
            memset(&kb[i], 0, sizeof(kb[i]));
        }
    }
    kb_loaded = true;
    logi("bc250: key guard: %d controller key backup(s) loaded\n", n);
}

static void kb_save(int i) {
    const btstack_tlv_t* impl;
    void* ctx;
    if (!kb_tlv(&impl, &ctx)) return;
    impl->store_tag(ctx, KEYBK_TAG(i), (const uint8_t*)&kb[i], sizeof(kb[i]));
}

// A controller became ready: its key is good, back it up (flash write only if it changed)
static void kb_note_ready(bd_addr_t addr) {
    if (!kb_loaded) kb_load();
    if (!kb_loaded) return;
    link_key_t key;
    link_key_type_t type;
    if (!gap_get_link_key_for_bd_addr(addr, key, &type)) return;
    int slot = -1;
    for (int i = 0; i < KEYBK_MAX && slot < 0; i++)
        if (kb[i].valid && bd_addr_cmp(kb[i].addr, addr) == 0) slot = i;
    if (slot >= 0 && memcmp(kb[slot].key, key, sizeof(key)) == 0 && kb[slot].type == (uint8_t)type) return;
    for (int i = 0; i < KEYBK_MAX && slot < 0; i++)
        if (!kb[i].valid) slot = i;
    if (slot < 0) {
        slot = kb_next;
        kb_next = (uint8_t)((kb_next + 1) % KEYBK_MAX);
    }
    bd_addr_copy(kb[slot].addr, addr);
    memcpy(kb[slot].key, key, sizeof(key));
    kb[slot].type = (uint8_t)type;
    kb[slot].valid = 1;
    kb_save(slot);
    logi("bc250: key guard: saved key backup for %s (slot %d)\n", bd_addr_to_str(addr), slot);
}

// Put back any backed-up key that Bluepad32 has deleted
static void kb_task(uint32_t t) {
    if (!kb_loaded) {
        kb_load();
        if (!kb_loaded) return;
    }
    if (t - kb_checked_at < KEY_GUARD_CHECK_MS) return;
    kb_checked_at = t;
    for (int i = 0; i < KEYBK_MAX; i++) {
        if (!kb[i].valid) continue;
        link_key_t key;
        link_key_type_t type;
        if (gap_get_link_key_for_bd_addr(kb[i].addr, key, &type)) continue;
        gap_store_link_key_for_bd_addr(kb[i].addr, kb[i].key, (link_key_type_t)kb[i].type);
        kb_restores++;
        logi("bc250: key guard: pairing key for %s was deleted - restored it (%lu so far)\n",
             bd_addr_to_str(kb[i].addr), (unsigned long)kb_restores);
    }
}

// ------------------------------------------------------------------ health check + recovery (v5)
// Every BT_HEALTH_PERIOD_MS we send HCI "Read Scan Enable" (harmless). The answer proves the radio is
// alive AND tells whether page scan is on, i.e. whether a paired controller can connect at all.
// We also watch incoming connection attempts: a controller that keeps knocking but never becomes
// ready means the stack is stuck even though the radio answers.
// Recovery = restart only Bluetooth: power the BTstack stack off, reset the CYW43 chip (reloads its
// firmware), run Bluepad32's setup again. The PSU line (GP15) is never touched, so the board keeps running.

#define OPC_READ_SCAN_ENABLE HCI_OPCODE(OGF_CONTROLLER_BASEBAND, 0x19)
static const hci_cmd_t cmd_read_scan_enable = {OPC_READ_SCAN_ENABLE, ""};

static btstack_packet_callback_registration_t health_cb_reg;
static volatile uint32_t h_answers;          // command-complete for our ping
static volatile uint8_t h_scan;              // last scan-enable value (bit 1 = page scan)
static volatile uint32_t h_conn_events;      // incoming connection requests / completions
static volatile uint32_t h_conn_failed;      // completions with error, or ACL dropped before ready
static volatile bool h_acl_pending;          // ACL up, HID not ready yet
static volatile uint32_t h_acl_up_at;

static uint32_t h_seen_answers, h_sent_at, h_last_try, h_last_ok, h_not_ready_since;
static bool h_waiting;
static uint8_t h_misses, h_page_off_count;
static uint32_t h_pings, h_fails_total;

// connection-attempt window
static uint32_t h_win_start, h_win_events0, h_win_failed0, h_win_ready0;

// recovery state
typedef enum { REC_IDLE, REC_HALTING, REC_STARTING } rec_state_t;
static rec_state_t rec = REC_IDLE;
static uint32_t rec_since, rec_window_start;
static uint8_t rec_in_window;
static uint32_t rec_total, rec_ok_total;
static bool bt_gave_up;
static const char* rec_reason = "";

static uint32_t ms_now(void) { return to_ms_since_boot(get_absolute_time()); }

static void health_hci_handler(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size) {
    ARG_UNUSED(channel);
    ARG_UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET) return;
    bd_addr_t a;
    switch (hci_event_packet_get_type(packet)) {
        case HCI_EVENT_COMMAND_COMPLETE:
            if (hci_event_command_complete_get_command_opcode(packet) == OPC_READ_SCAN_ENABLE) {
                const uint8_t* r = hci_event_command_complete_get_return_parameters(packet);
                h_scan = (r[0] == 0) ? r[1] : 0xFF;
                h_answers++;
            }
            break;
        case HCI_EVENT_CONNECTION_REQUEST:
            hci_event_connection_request_get_bd_addr(packet, a);
            h_conn_events++;
            logi("bc250: BT connection request from %s\n", bd_addr_to_str(a));
            break;
        case HCI_EVENT_CONNECTION_COMPLETE: {
            uint8_t st = hci_event_connection_complete_get_status(packet);
            hci_event_connection_complete_get_bd_addr(packet, a);
            h_conn_events++;
            if (st) {
                h_conn_failed++;
                logi("bc250: BT connection from %s failed, status 0x%02x\n", bd_addr_to_str(a), st);
            } else {
                h_acl_pending = true;
                h_acl_up_at = ms_now();
                logi("bc250: BT link up with %s\n", bd_addr_to_str(a));
            }
            break;
        }
        case HCI_EVENT_DISCONNECTION_COMPLETE:
            logi("bc250: BT link closed, reason 0x%02x\n", hci_event_disconnection_complete_get_reason(packet));
            if (h_acl_pending) h_conn_failed++;   // dropped before the controller became ready
            h_acl_pending = false;
            break;
        default:
            break;
    }
}

static void health_reset_counters(uint32_t t) {
    h_waiting = false;
    h_misses = 0;
    h_page_off_count = 0;
    h_last_ok = t;
    h_last_try = t;
    h_not_ready_since = 0;
    h_acl_pending = false;
    h_win_start = 0;
}

bool bt_restarting(void) { return rec != REC_IDLE; }

static void bt_restart(uint32_t t, const char* why) {
    if (rec != REC_IDLE || bt_gave_up) return;
    if (!rec_window_start || t - rec_window_start >= BT_RESTART_WINDOW_MS) {
        rec_window_start = t;
        rec_in_window = 0;
    }
    if (rec_in_window >= BT_RESTART_MAX_IN_WINDOW) {
        logi("bc250: BT restart: %s - but already restarted %u times recently, giving up (Pico restarts after power-off)\n",
             why, rec_in_window);
        bt_gave_up = true;
        return;
    }
    rec_in_window++;
    rec_total++;
    rec_reason = why;
    logi("bc250: BT restart #%lu: %s - stopping Bluetooth\n", (unsigned long)rec_total, why);
    for (int i = 0; i < MAX_PADS; i++)
        if (bs[i].dev) uni_hid_device_disconnect(bs[i].dev);
    hci_power_control(HCI_POWER_OFF);
    rec = REC_HALTING;
    rec_since = t;
}

void bt_restart_now(void) { bt_restart(ms_now(), "requested over the log port"); }

static bool health_fail(const char* why) {
    h_fails_total++;
    bt_restart(ms_now(), why);
    return !bt_gave_up;
}

// Returns false once Bluetooth is beyond self-repair (main.c then restarts the Pico, PSU off only).
bool bt_health_task(uint32_t t) {
    // ---- recovery in progress
    if (rec == REC_HALTING) {
        if (hci_get_state() == HCI_STATE_OFF) {
            logi("bc250: BT restart: stack off after %lu ms, resetting the radio chip\n", (unsigned long)(t - rec_since));
            cyw43_deinit(&cyw43_state);   // WL_REG_ON low; next use powers it up and reloads the firmware
            for (int i = 0; i < MAX_PADS; i++)   // in case a disconnect callback never came
                if (pads[i].used || bs[i].dev) p_on_device_disconnected_slot(i);
            uni_bt_setup();               // Bluepad32 setup again (supports repeated calls) -> HCI power on
            rec = REC_STARTING;
            rec_since = t;
        } else if (t - rec_since >= BT_RESTART_STOP_MS) {
            logi("bc250: BT restart: stack did not stop within %u ms - giving up\n", BT_RESTART_STOP_MS);
            rec = REC_IDLE;
            bt_gave_up = true;
        }
        return !bt_gave_up;
    }
    if (rec == REC_STARTING) {
        if (uni_bt_setup_is_ready() && hci_get_state() == HCI_STATE_WORKING) {
            rec = REC_IDLE;
            rec_ok_total++;
            logi("bc250: BT restart: Bluetooth is back after %lu ms\n", (unsigned long)(t - rec_since));
            health_reset_counters(t);
        } else if (t - rec_since >= BT_RESTART_START_MS) {
            logi("bc250: BT restart: Bluetooth did not come back within %u ms - giving up\n", BT_RESTART_START_MS);
            rec = REC_IDLE;
            bt_gave_up = true;
        }
        return !bt_gave_up;
    }
    if (bt_gave_up) return false;

    // ---- stack must be up (Bluepad32 treats any HCI event as "next setup step", so no pings before)
    if (!uni_bt_setup_is_ready() || hci_get_state() != HCI_STATE_WORKING) {
        if (!h_not_ready_since) h_not_ready_since = t ? t : 1;
        if (t - h_not_ready_since >= BT_HEALTH_NOT_READY_MS) {
            h_not_ready_since = 0;
            return health_fail("stack not running");
        }
        return true;
    }
    h_not_ready_since = 0;
    if (!h_last_ok) h_last_ok = t;
    kb_task(t);   // v6: put back pairing keys Bluepad32 deleted

    // ---- ping answers
    if (h_waiting) {
        if (h_answers != h_seen_answers) {
            h_seen_answers = h_answers;
            h_waiting = false;
            h_misses = 0;
            h_last_ok = t;
            int connected = 0;
            for (int i = 0; i < MAX_PADS; i++) connected += pads[i].used;
            if (h_scan != 0xFF && !(h_scan & 0x02) && connected < MAX_PADS) {
                h_page_off_count++;
                logi("bc250: BT health: page scan is OFF (scan_enable=0x%02x) - controllers can't connect\n", h_scan);
                if (h_page_off_count == 1) {
                    gap_connectable_control(1);   // cheap fix first
                } else {
                    h_page_off_count = 0;
                    return health_fail("page scan stayed off");
                }
            } else {
                h_page_off_count = 0;
            }
        } else if (t - h_sent_at >= BT_HEALTH_TIMEOUT_MS) {
            h_waiting = false;
            h_misses++;
            logi("bc250: BT health: no answer (%u in a row)\n", h_misses);
        }
    }
    if (!h_waiting && t - h_last_try >= BT_HEALTH_PERIOD_MS) {
        h_last_try = t;
        if (hci_can_send_command_packet_now() && hci_send_cmd(&cmd_read_scan_enable) == ERROR_CODE_SUCCESS) {
            h_seen_answers = h_answers;
            h_waiting = true;
            h_sent_at = t;
            h_pings++;
        } else {
            h_misses++;
            logi("bc250: BT health: HCI busy (%u in a row)\n", h_misses);
        }
    }
    if (h_misses >= BT_HEALTH_MAX_MISSES) {
        h_misses = 0;
        return health_fail("radio stopped answering");
    }

    // ---- a controller links up but never becomes ready
    if (h_acl_pending && t - h_acl_up_at >= BT_CONNECT_STUCK_MS) {
        h_acl_pending = false;
        return health_fail("controller linked up but never became ready");
    }
    // ---- a controller keeps knocking without getting in
    if (!h_win_start && (h_conn_events != h_win_events0 || h_conn_failed != h_win_failed0)) {
        h_win_start = t ? t : 1;
        h_win_events0 = h_conn_events;
        h_win_failed0 = h_conn_failed;
        h_win_ready0 = h_ready_events;
    }
    if (h_win_start) {
        if (h_ready_events != h_win_ready0) {
            h_win_start = 0;   // someone got in: fine
            h_win_events0 = h_conn_events;
            h_win_failed0 = h_conn_failed;
        } else if (h_conn_failed - h_win_failed0 >= BT_CONNECT_FAILS_MAX) {
            h_win_start = 0;
            h_win_events0 = h_conn_events;
            h_win_failed0 = h_conn_failed;
            return health_fail("controller keeps trying to connect but fails");
        } else if (t - h_win_start >= BT_CONNECT_WINDOW_MS) {
            h_win_start = 0;
            h_win_events0 = h_conn_events;
            h_win_failed0 = h_conn_failed;
        }
    }
    return true;
}

void bt_health_status(uint32_t t, char* buf, int len) {
    int n = 0;
    for (int i = 0; i < MAX_PADS; i++) n += pads[i].used;
    snprintf(buf, (size_t)len,
             "bt ready=%d hci=%d scan=0x%02x pings=%lu last_ok=%lus ago conn_try=%lu conn_fail=%lu bt_restarts=%lu/%lu keys_restored=%lu%s pads=%d",
             uni_bt_setup_is_ready(), (int)hci_get_state(), h_scan, (unsigned long)h_pings,
             h_last_ok ? (unsigned long)((t - h_last_ok) / 1000) : 0ul, (unsigned long)h_conn_events,
             (unsigned long)h_conn_failed, (unsigned long)rec_ok_total, (unsigned long)rec_total,
             (unsigned long)kb_restores, bt_gave_up ? " GAVE-UP" : "", n);
}

// ------------------------------------------------------------------ API

void bt_bridge_start(void) {
    uni_platform_set_custom(&platform);
    uni_init(0, NULL);
    health_cb_reg.callback = health_hci_handler;
    hci_add_event_handler(&health_cb_reg);
}

void bt_disconnect_all(void) {
    for (int i = 0; i < MAX_PADS; i++)
        if (bs[i].dev) uni_hid_device_disconnect(bs[i].dev);
}

void bt_rumble(uint8_t slot, uint8_t strong, uint8_t weak) {
    if (slot >= MAX_PADS) return;
    uni_hid_device_t* d = bs[slot].dev;
    if (!d || !d->report_parser.play_dual_rumble) return;
    uint16_t dur = (strong || weak) ? RUMBLE_DURATION_MS : 1;
    d->report_parser.play_dual_rumble(d, 0, dur, weak, strong);
}
