// USB descriptors for every personality (built at runtime for the connected pads),
// HID callbacks and native report forwarding.
#include "usb_device.h"

#include <stdio.h>
#include <string.h>

#include "pads.h"
#include "pico/unique_id.h"
#include "tusb.h"
#include "usb_xinput.h"

usb_personality_t usb_active = USB_P_IDLE;
uint8_t usb_n;
int8_t usb_map[MAX_PADS];

static usb_personality_t target_p = USB_P_IDLE;
static uint8_t target_mask, active_mask;
static bool switching;
static uint32_t switch_at;
static uint16_t active_pid;
static bool allowed;   // USB connection enabled (PSU on and BC-250 USB powered)

// ============================================================ HID report descriptors

// Power key: Generic Desktop / System Control. 0x01 = System Power Down -> KEY_POWER
static const uint8_t rd_power[] = { TUD_HID_REPORT_DESC_SYSTEM_CONTROL() };

#define RD_GAMEPAD_BEGIN 0x05, 0x01, 0x09, 0x05, 0xA1, 0x01
#define RD_END 0xC0
// input report: id, 6 axes (X Y Z Rz Rx Ry) + n vendor bytes
#define RD_INPUT(id, vendor_n) \
    0x85, id, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35, 0x09, 0x33, 0x09, 0x34, \
    0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x06, 0x81, 0x02, \
    0x06, 0x00, 0xFF, 0x09, 0x20, 0x95, vendor_n, 0x81, 0x02
#define RD_VENDOR_INPUT(id, n)  0x85, id, 0x06, 0x00, 0xFF, 0x09, 0x21, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, n, 0x81, 0x02
#define RD_OUTPUT(id, n)        0x85, id, 0x06, 0x00, 0xFF, 0x09, 0x22, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, n, 0x91, 0x02
#define RD_FEATURE(id, n)       0x85, id, 0x06, 0x00, 0xFF, 0x09, 0x23, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, n, 0xB1, 0x02

// Sizes are data bytes (without report id) as used by Linux hid-playstation / hid-nintendo and Steam
static const uint8_t rd_ds5[] = {
    RD_GAMEPAD_BEGIN,
    RD_INPUT(0x01, 57),        // 63 bytes
    RD_OUTPUT(0x02, 63),
    RD_FEATURE(0x05, 40),      // calibration
    RD_FEATURE(0x09, 19),      // pairing info (MAC)
    RD_FEATURE(0x20, 63),      // firmware info
    RD_END,
};

static const uint8_t rd_ds4[] = {
    RD_GAMEPAD_BEGIN,
    RD_INPUT(0x01, 57),        // 63 bytes
    RD_OUTPUT(0x05, 31),
    RD_FEATURE(0x02, 36),      // calibration (USB layout)
    RD_FEATURE(0x12, 15),      // pairing info (MAC)
    RD_FEATURE(0xA3, 48),      // firmware info
    RD_END,
};

static const uint8_t rd_switch[] = {
    RD_GAMEPAD_BEGIN,
    RD_INPUT(0x30, 57),        // full report
    RD_VENDOR_INPUT(0x21, 63), // sub-command reply
    RD_VENDOR_INPUT(0x81, 63), // USB command reply
    RD_OUTPUT(0x01, 63),       // rumble + sub-command
    RD_OUTPUT(0x10, 63),       // rumble
    RD_OUTPUT(0x80, 63),       // USB command
    RD_OUTPUT(0x82, 63),
    RD_END,
};

// ============================================================ configuration descriptor (built per enumeration)

#define ATTR TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP
static uint8_t cfg[TUD_CONFIG_DESC_LEN + MAX_PADS * XINPUT_DESC_LEN + TUD_HID_DESC_LEN + 16];
static uint16_t cfg_len;

static void cfg_add(const uint8_t* d, uint16_t n) {
    memcpy(&cfg[cfg_len], d, n);
    cfg_len += n;
}

static void build_config(void) {
    cfg_len = TUD_CONFIG_DESC_LEN;   // header filled in at the end
    uint8_t itfs = 0;
    uint16_t power_ma = 100;

    if (usb_active == USB_P_IDLE || usb_active == USB_P_XBOX) {
        if (usb_active == USB_P_XBOX) {
            for (uint8_t k = 0; k < usb_n; k++) {
                uint8_t d[] = { XINPUT_DESCRIPTOR(k, (uint8_t)(0x81 + k), (uint8_t)(0x01 + k)) };
                cfg_add(d, sizeof(d));
            }
            itfs = usb_n;
        }
        uint8_t h[] = { TUD_HID_DESCRIPTOR(itfs, 4, HID_ITF_PROTOCOL_NONE, sizeof(rd_power), (uint8_t)(0x81 + itfs), 8, 10) };
        cfg_add(h, sizeof(h));
        itfs++;
    } else {
        uint16_t rd_len = usb_active == USB_P_DS4 ? sizeof(rd_ds4) : usb_active == USB_P_DS5 ? sizeof(rd_ds5) : sizeof(rd_switch);
        uint8_t interval = usb_active == USB_P_SWITCH ? 8 : 1;
        for (uint8_t k = 0; k < usb_n; k++) {
            uint8_t h[] = { TUD_HID_INOUT_DESCRIPTOR(k, 0, HID_ITF_PROTOCOL_NONE, rd_len, (uint8_t)(0x01 + k), (uint8_t)(0x81 + k), 64, interval) };
            cfg_add(h, sizeof(h));
        }
        itfs = usb_n;
        power_ma = 500;
    }
    uint8_t c[] = { TUD_CONFIG_DESCRIPTOR(1, itfs, 0, cfg_len, ATTR, power_ma) };
    memcpy(cfg, c, sizeof(c));
}

uint8_t const* tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return cfg;
}

uint8_t const* tud_hid_descriptor_report_cb(uint8_t instance) {
    (void)instance;
    switch (usb_active) {
        case USB_P_DS4: return rd_ds4;
        case USB_P_DS5: return rd_ds5;
        case USB_P_SWITCH: return rd_switch;
        default: return rd_power;
    }
}

// ============================================================ device descriptor + strings

static tusb_desc_device_t desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

static const char* str_manufacturer = "Raspberry Pi";
static const char* str_product = "BC-250 Power Controller";

static void build_device(void) {
    uint16_t vid = 0x2E8A, pid = 0x10BC, bcd = 0x0100;   // idle: Raspberry Pi VID, private PID
    str_manufacturer = "Raspberry Pi";
    str_product = "BC-250 Power Controller";
    switch (usb_active) {
        case USB_P_XBOX:
            vid = 0x045E; pid = 0x028E; bcd = 0x0114;
            str_manufacturer = "Microsoft"; str_product = "Controller";
            break;
        case USB_P_DS4:
            vid = 0x054C; pid = active_pid;
            str_manufacturer = "Sony Interactive Entertainment"; str_product = "Wireless Controller";
            break;
        case USB_P_DS5:
            vid = 0x054C; pid = active_pid;
            str_manufacturer = "Sony Interactive Entertainment";
            str_product = (pid == 0x0DF2) ? "DualSense Edge Wireless Controller" : "DualSense Wireless Controller";
            break;
        case USB_P_SWITCH:
            vid = 0x057E; pid = 0x2009; bcd = 0x0200;
            str_manufacturer = "Nintendo Co., Ltd."; str_product = "Pro Controller";
            break;
        default:
            break;
    }
    desc_device.idVendor = vid;
    desc_device.idProduct = pid;
    desc_device.bcdDevice = bcd;
}

uint8_t const* tud_descriptor_device_cb(void) { return (uint8_t const*)&desc_device; }

static uint16_t desc_str[48];

uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    uint8_t n;
    if (index == 0) {
        desc_str[1] = 0x0409;
        n = 1;
    } else {
        char serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
        const char* s;
        switch (index) {
            case 1: s = str_manufacturer; break;
            case 2: s = str_product; break;
            case 3: pico_get_unique_board_id_string(serial, sizeof(serial)); s = serial; break;
            case 4: s = "BC-250 Power Key"; break;
            default: return NULL;
        }
        n = (uint8_t)strlen(s);
        if (n > 47) n = 47;
        for (uint8_t i = 0; i < n; i++) desc_str[1 + i] = (uint8_t)s[i];
    }
    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * n + 2));
    return desc_str;
}

// ============================================================ HID GET/SET report (native personalities)

static pad_t* pad_for_instance(uint8_t instance) {
    if (instance >= usb_n || usb_map[instance] < 0) return NULL;
    return &pads[usb_map[instance]];
}

// Copy a cached BT feature report (id first) into buf, padded with zeros to `want` data bytes
static uint16_t give(const pkt_t* p, uint8_t* buf, uint16_t want, uint16_t reqlen) {
    if (want > reqlen) want = reqlen;
    memset(buf, 0, want);
    if (p->len > 1) memcpy(buf, p->data + 1, (p->len - 1u < want) ? p->len - 1u : want);
    return want;
}

static void mac_le(const pad_t* p, uint8_t* out) {   // controller MAC, little endian (as Sony reports it)
    for (int i = 0; i < 6; i++) out[i] = p->mac[5 - i];
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t id, hid_report_type_t type, uint8_t* buf, uint16_t reqlen) {
    pad_t* p = pad_for_instance(instance);
    if (!p || type != HID_REPORT_TYPE_FEATURE) return 0;

    if (usb_active == USB_P_DS5) {
        switch (id) {
            case 0x05: return give(&p->feat_calib, buf, 40, reqlen);
            case 0x20: return give(&p->feat_firmware, buf, 63, reqlen);
            case 0x09: {
                uint16_t n = give(&p->feat_pairing, buf, 19, reqlen);
                if (p->feat_pairing.len < 7 && n >= 6) mac_le(p, buf);
                return n;
            }
        }
    } else if (usb_active == USB_P_DS4) {
        switch (id) {
            case 0x02: {   // USB calibration layout differs from the BT one (0x05): reorder gyro +/-
                uint16_t n = reqlen < 36 ? reqlen : 36;
                uint8_t u[36] = {0};
                const uint8_t* b = p->feat_calib.data;   // b[0] = 0x05
                if (p->feat_calib.len >= 37) {
                    memcpy(&u[0], &b[1], 6);     // gyro bias pitch/yaw/roll
                    memcpy(&u[6], &b[7], 2);     // pitch +
                    memcpy(&u[8], &b[13], 2);    // pitch -
                    memcpy(&u[10], &b[9], 2);    // yaw +
                    memcpy(&u[12], &b[15], 2);   // yaw -
                    memcpy(&u[14], &b[11], 2);   // roll +
                    memcpy(&u[16], &b[17], 2);   // roll -
                    memcpy(&u[18], &b[19], 18);  // gyro speed +/-, accel x/y/z +/-
                }
                memcpy(buf, u, n);
                return n;
            }
            case 0x12: {
                uint16_t n = reqlen < 15 ? reqlen : 15;
                uint8_t u[15] = {0};
                mac_le(p, u);
                memcpy(buf, u, n);
                return n;
            }
            case 0xA3: return give(&p->feat_firmware, buf, 48, reqlen);
        }
    }
    return 0;
}

static void switch_usb_command(pad_t* p, const uint8_t* d, uint16_t len) {
    if (len < 2) return;
    uint8_t r[64] = {0x81, d[1]};
    if (d[1] == 0x01) {          // status: 00 03 <MAC, little endian>
        r[3] = 0x03;
        mac_le(p, &r[4]);
    }
    ring_push(&p->local, r, 64);
}

static void native_output(pad_t* p, const uint8_t* d, uint16_t len) {
    if (len < 1) return;
    uint8_t n = (uint8_t)(len > 64 ? 64 : len);
    switch (usb_active) {
        case USB_P_DS5:
            if (d[0] == 0x02) ring_push(&p->to_bt, d, n);
            break;
        case USB_P_DS4:
            if (d[0] == 0x05) ring_push(&p->to_bt, d, n);
            break;
        case USB_P_SWITCH:
            if (d[0] == 0x80) switch_usb_command(p, d, len);
            else if (d[0] == 0x01 || d[0] == 0x10 || d[0] == 0x11) ring_push(&p->to_bt, d, n);
            break;
        default:
            break;
    }
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t id, hid_report_type_t type, uint8_t const* buf, uint16_t len) {
    pad_t* p = pad_for_instance(instance);
    if (!p || usb_active < USB_P_DS4) return;
    if (id == 0) {                               // interrupt OUT: id is buf[0]
        native_output(p, buf, len);
    } else if (type == HID_REPORT_TYPE_OUTPUT) { // control SET_REPORT: id was stripped
        uint8_t tmp[64];
        if (len > 63) len = 63;
        tmp[0] = id;
        memcpy(&tmp[1], buf, len);
        native_output(p, tmp, (uint16_t)(len + 1));
    }
    // feature SET_REPORTs are ignored
}

// ============================================================ personality switching

void usb_set_target(usb_personality_t p, uint8_t mask) {
    target_p = p;
    target_mask = (p == USB_P_IDLE) ? 0 : mask;
}
usb_personality_t usb_target_personality(void) { return target_p; }
uint8_t usb_active_mask(void) { return active_mask; }
bool usb_switching(void) { return switching || target_p != usb_active || target_mask != active_mask; }
bool usb_has_power_key(void) { return !usb_switching() && (usb_active == USB_P_IDLE || usb_active == USB_P_XBOX); }

static const char* const pname[] = {"idle (power key)", "Xbox 360", "DualShock 4", "DualSense", "Switch Pro"};

static uint16_t pick_pid(void) {   // all the same real PID -> use it, otherwise the standard one
    uint16_t std = usb_active == USB_P_DS4 ? 0x09CC : 0x0CE6;
    uint16_t pid = 0;
    for (uint8_t k = 0; k < usb_n; k++) {
        uint16_t q = pads[usb_map[k]].pid;
        if (pid == 0) pid = q;
        else if (q != pid) return std;
    }
    if (usb_active == USB_P_DS4 && pid != 0x05C4 && pid != 0x09CC) return std;
    if (usb_active == USB_P_DS5 && pid != 0x0CE6 && pid != 0x0DF2) return std;
    return pid ? pid : std;
}

void usb_task(uint32_t t) {
    if (!switching && (target_p != usb_active || target_mask != active_mask)) {
        tud_disconnect();          // looks like the cable was pulled
        switching = true;
        switch_at = t;
    } else if (switching && t - switch_at >= 400) {
        usb_active = target_p;
        active_mask = target_mask;
        usb_n = 0;
        for (int i = 0; i < MAX_PADS; i++) {
            usb_map[i] = -1;
            ring_clear(&pads[i].local);
            ring_clear(&pads[i].to_bt);
            ring_clear(&pads[i].to_usb);
            pads[i].xi_dirty = true;
        }
        for (int i = 0; i < MAX_PADS; i++)
            if (active_mask & (1u << i)) usb_map[usb_n++] = (int8_t)i;
        active_pid = (usb_active == USB_P_DS4 || usb_active == USB_P_DS5) ? pick_pid() : 0;
        build_device();
        build_config();
        printf("bc250: USB now presents: %u x %s\n", usb_n, pname[usb_active]);
        if (allowed) tud_connect();   // re-plug with the new identity (only if the BC-250 is powered)
        switching = false;
    }

    // Native personalities: push raw input reports to the host, one HID interface per controller
    if (!switching && usb_active >= USB_P_DS4 && tud_ready()) {
        for (uint8_t k = 0; k < usb_n; k++) {
            pad_t* p = &pads[usb_map[k]];
            pkt_t pk;
            while (tud_hid_n_ready(k)) {
                if (!ring_pop(&p->local, &pk) && !ring_pop(&p->to_usb, &pk)) break;
                if (!tud_hid_n_report(k, 0, pk.data, 64)) break;
            }
        }
    }
}

void usb_allow(bool on) {
    if (on == allowed) return;
    allowed = on;
    if (on) {
        if (!switching) tud_connect();
        printf("bc250: USB connected\n");
    } else {
        tud_disconnect();   // pull-up off: nothing leaks into an unpowered BC-250
        printf("bc250: USB disconnected\n");
    }
}
bool usb_allowed(void) { return allowed; }

// Called once at boot, before tusb_init()
void usb_device_init(void) {
    for (int i = 0; i < MAX_PADS; i++) usb_map[i] = -1;
    build_device();
    build_config();
}
