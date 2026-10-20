#include "usb_xinput.h"

#include <string.h>

#include "config.h"
#include "device/usbd_pvt.h"

typedef struct {
    uint8_t ep_in, ep_out;
    bool opened;
    CFG_TUSB_MEM_ALIGN uint8_t in_buf[32];
    CFG_TUSB_MEM_ALIGN uint8_t out_buf[32];
} xinput_itf_t;

static xinput_itf_t itfs[MAX_PADS];

static void xi_init(void) { memset(itfs, 0, sizeof(itfs)); }
static bool xi_deinit(void) { return true; }
static void xi_reset(uint8_t rhport) { (void)rhport; memset(itfs, 0, sizeof(itfs)); }

static uint16_t xi_open(uint8_t rhport, tusb_desc_interface_t const* d, uint16_t max_len) {
    if (d->bInterfaceClass != 0xFF || d->bInterfaceSubClass != 0x5D || d->bInterfaceProtocol != 0x01) return 0;
    uint8_t idx = d->bInterfaceNumber;
    if (idx >= MAX_PADS || max_len < XINPUT_DESC_LEN) return 0;

    uint8_t const* p = tu_desc_next(d);             // vendor 0x21 descriptor
    if (tu_desc_type(p) != 0x21) return 0;
    p = tu_desc_next(p);                            // first endpoint
    xinput_itf_t* x = &itfs[idx];
    if (!usbd_open_edpt_pair(rhport, p, 2, TUSB_XFER_INTERRUPT, &x->ep_out, &x->ep_in)) return 0;
    x->opened = true;
    usbd_edpt_xfer(rhport, x->ep_out, x->out_buf, sizeof(x->out_buf));
    return XINPUT_DESC_LEN;
}

static bool xi_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const* req) {
    (void)rhport; (void)stage; (void)req;
    return false;  // Linux xpad does not need any vendor control requests
}

static bool xi_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t len) {
    for (uint8_t i = 0; i < MAX_PADS; i++) {
        xinput_itf_t* x = &itfs[i];
        if (!x->opened || ep_addr != x->ep_out) continue;
        // Rumble: 00 08 00 <strong> <weak> 00 00 00   (LED reports 01 03 xx are ignored)
        if (result == XFER_RESULT_SUCCESS && len >= 5 && x->out_buf[0] == 0x00 && x->out_buf[1] == 0x08)
            xinput_rumble_cb(i, x->out_buf[3], x->out_buf[4]);
        usbd_edpt_xfer(rhport, x->ep_out, x->out_buf, sizeof(x->out_buf));
        return true;
    }
    return true;  // IN transfer finished - nothing to do
}

static const usbd_class_driver_t xinput_driver = {
#if CFG_TUSB_DEBUG >= 2
    .name = "XINPUT",
#endif
    .init = xi_init,
    .deinit = xi_deinit,
    .reset = xi_reset,
    .open = xi_open,
    .control_xfer_cb = xi_control_xfer_cb,
    .xfer_cb = xi_xfer_cb,
    .sof = NULL,
};

usbd_class_driver_t const* usbd_app_driver_get_cb(uint8_t* count) {
    *count = 1;
    return &xinput_driver;
}

bool xinput_ready(uint8_t idx) {
    return idx < MAX_PADS && tud_ready() && itfs[idx].opened && !usbd_edpt_busy(0, itfs[idx].ep_in);
}

bool xinput_send(uint8_t idx, const xinput_report_t* r) {
    if (!xinput_ready(idx)) return false;
    xinput_itf_t* x = &itfs[idx];
    if (!usbd_edpt_claim(0, x->ep_in)) return false;
    memcpy(x->in_buf, r, sizeof(*r));
    bool ok = usbd_edpt_xfer(0, x->ep_in, x->in_buf, sizeof(*r));
    if (!ok) usbd_edpt_release(0, x->ep_in);
    return ok;
}
