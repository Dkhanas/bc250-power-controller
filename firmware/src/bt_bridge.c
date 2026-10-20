#include "bt_bridge.h"

#include <stdio.h>
#include <string.h>
#include <btstack.h>
#include <uni.h>

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

static void p_on_device_disconnected(uni_hid_device_t* d) {
    int i = slot_of(d);
    if (i < 0) return;
    pads[i].used = false;
    pads[i].fetching = false;
    ring_clear(&pads[i].to_usb);
    memset(&bs[i], 0, sizeof(bs[i]));
    logi("bc250: controller in slot %d disconnected\n", i);
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

// ------------------------------------------------------------------ health check
// The radio has a separate Bluetooth core. If it (or the BTstack run loop) locks up, the button
// still works but no controller can reach us. Every BT_HEALTH_PERIOD_MS we send HCI
// "Read Local Version Information" (no side effects) and wait for its Command Complete event.

static btstack_packet_callback_registration_t health_cb_reg;
static volatile uint32_t health_answers;       // incremented by the HCI event handler
static uint32_t health_seen_answers;
static bool health_waiting;
static uint32_t health_sent_at, health_last_try, health_last_ok, health_not_ready_since;
static uint8_t health_misses;
static uint32_t health_pings, health_fails_total;

static void health_hci_handler(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size) {
    ARG_UNUSED(channel);
    ARG_UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) == HCI_EVENT_COMMAND_COMPLETE &&
        hci_event_command_complete_get_command_opcode(packet) == HCI_OPCODE_HCI_READ_LOCAL_VERSION_INFORMATION)
        health_answers++;
}

static bool health_fail(uint32_t t, const char* why) {
    (void)t;
    health_fails_total++;
    logi("bc250: BT health: %s\n", why);
    return false;
}

bool bt_health_task(uint32_t t) {
    // Bluepad32 setup still running (it treats any HCI event as "next step", so don't ping yet)
    if (!uni_bt_setup_is_ready() || hci_get_state() != HCI_STATE_WORKING) {
        if (!health_not_ready_since) health_not_ready_since = t ? t : 1;
        if (t - health_not_ready_since >= BT_HEALTH_NOT_READY_MS)
            return health_fail(t, "stack not ready for too long");
        return true;
    }
    health_not_ready_since = 0;
    if (!health_last_ok) health_last_ok = t;

    if (health_waiting) {
        if (health_answers != health_seen_answers) {   // answered
            health_seen_answers = health_answers;
            health_waiting = false;
            health_misses = 0;
            health_last_ok = t;
        } else if (t - health_sent_at >= BT_HEALTH_TIMEOUT_MS) {
            health_waiting = false;
            health_misses++;
            logi("bc250: BT health: no answer (%u in a row)\n", health_misses);
        }
    }

    if (!health_waiting && t - health_last_try >= BT_HEALTH_PERIOD_MS) {
        health_last_try = t;
        if (hci_can_send_command_packet_now()) {
            health_seen_answers = health_answers;
            if (hci_send_cmd(&hci_read_local_version_information) == ERROR_CODE_SUCCESS) {
                health_waiting = true;
                health_sent_at = t;
                health_pings++;
            } else {
                health_misses++;
            }
        } else {
            health_misses++;   // command queue stuck counts as a miss too
            logi("bc250: BT health: HCI busy (%u in a row)\n", health_misses);
        }
    }

    if (health_misses >= BT_HEALTH_MAX_MISSES) return health_fail(t, "radio stopped answering");
    return true;
}

void bt_health_status(uint32_t t, char* buf, int len) {
    int n = 0;
    for (int i = 0; i < MAX_PADS; i++) n += pads[i].used;
    snprintf(buf, (size_t)len, "bt ready=%d hci=%d pings=%lu misses=%u last_ok=%lus ago fails=%lu pads=%d",
             uni_bt_setup_is_ready(), (int)hci_get_state(), (unsigned long)health_pings, health_misses,
             health_last_ok ? (unsigned long)((t - health_last_ok) / 1000) : 0ul,
             (unsigned long)health_fails_total, n);
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
