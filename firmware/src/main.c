// BC-250 power controller - Raspberry Pi Pico 2 W
//
//  Case button  short press : PSU on            (board boots via AUTO_PWRON)
//               short press : clean shutdown    (USB "Power" key -> SteamOS powers off)
//               hold 5 s    : force PSU off
//  TPM pin 9 low for 5 s while running -> PSU off (SteamOS finished shutting down)
//  Bluetooth controller connects / PS-Xbox-Home pressed while off -> PSU on
//  Bluetooth controllers are passed to the BC-250 as wired Xbox 360 pads (rumble included)
//  Button LED: off/standby, slow blink = starting, on = running, fast blink = shutting down
//  v4: if Bluetooth stops answering (or every 6 h idle) the Pico restarts itself - PSU off only
//  v5: Bluetooth trouble is first fixed by restarting only Bluetooth (board keeps running);
//      event log readable on the BC-250 over USB serial (/dev/ttyACM0) while no controller is connected
//  v6: pairing key guard - restores a controller's key when Bluepad32 deletes it after a failed reconnect
//  v7: pairing window - new controllers only while open (hold the case button 2 s, release); otherwise
//      controllers in pairing mode are ignored, so pairing one with a phone/PC isn't taken over
//  v8: Wi-Fi - web page (http://bc250.local) and Home Assistant (MQTT); low-battery warning

#include <stdio.h>

#include "bt_bridge.h"
#include "log.h"
#include "net.h"
#include "settings.h"
#include "pads.h"
#include "config.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "hardware/structs/watchdog.h"
#include "hardware/watchdog.h"
#include "pico/async_context.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "tusb.h"
#include "usb_device.h"
#include "usb_xinput.h"

typedef enum { ST_OFF, ST_STARTING, ST_RUNNING, ST_STOPPING } state_t;
static const char* const state_name[] = {"OFF", "STARTING", "RUNNING", "STOPPING"};

static state_t state = ST_OFF;
static uint32_t state_since;
static uint32_t psu_off_at;          // for BT wake cooldown
static bool psu_was_ever_on;

// ------------------------------------------------------------------ helpers
static inline uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

static void set_state(state_t s) {
    if (s == state) return;
    printf("bc250: %s -> %s\n", state_name[state], state_name[s]);
    net_notify_change();
    net_console_on(s != ST_OFF);
    state = s;
    state_since = now_ms();
}

static void psu(bool on) { gpio_put(PIN_PSU, on); }

// ------------------------------------------------------------------ debounced inputs
typedef struct {
    bool stable, last_raw;
    uint32_t changed_at;
} debounce_t;

static bool debounce(debounce_t* d, bool raw, uint32_t t, uint32_t ms) {
    if (raw != d->last_raw) {
        d->last_raw = raw;
        d->changed_at = t;
    } else if (raw != d->stable && t - d->changed_at >= ms) {
        d->stable = raw;
        return true;   // edge
    }
    return false;
}

static debounce_t btn_db, tpm_db;
static bool battery_low_any;   // v8: a connected controller is low (LED dip)
static uint32_t btn_down_at;
static bool btn_long_fired;
static uint32_t tpm_low_since;

typedef enum { BTN_NONE, BTN_SHORT, BTN_LONG, BTN_PAIR, BTN_WIFI } btn_event_t;

static btn_event_t poll_button(uint32_t t) {
    bool pressed = !gpio_get(PIN_BUTTON);
    if (debounce(&btn_db, pressed, t, BUTTON_DEBOUNCE_MS)) {
        if (btn_db.stable) {
            btn_down_at = t;
            btn_long_fired = false;
        } else if (!btn_long_fired) {
            uint32_t held = t - btn_down_at;
            if (state == ST_OFF && held >= WIFI_HOLD_MS) return BTN_WIFI;   // v8: console off, 8 s: Wi-Fi on
            if (held >= PAIR_HOLD_MS) {
                // 2 s+ hold: pairing window (with the PSU on, only up to PAIR_HOLD_MAX_MS; 5 s = force off)
                if (state == ST_OFF || held < PAIR_HOLD_MAX_MS) return BTN_PAIR;
                return BTN_NONE;
            }
            return BTN_SHORT;
        }
    }
    // 5 s force-off only matters with the PSU on; with it off, longer holds are pairing / Wi-Fi (on release)
    if (state != ST_OFF && btn_db.stable && !btn_long_fired && t - btn_down_at >= BUTTON_LONG_PRESS_MS) {
        btn_long_fired = true;
        return BTN_LONG;
    }
    return BTN_NONE;
}

// ------------------------------------------------------------------ USB power key
typedef enum { KEY_IDLE, KEY_WANT_PRESS, KEY_HELD, KEY_WANT_RELEASE } key_phase_t;
static key_phase_t key_phase;
static uint32_t key_pressed_at;

static bool key_identity;        // native controller active: re-enumerate as "power key" device first
static uint32_t usb_mounted_at;

static bool request_power_key(void) {
    if (!usb_has_power_key()) {
        // A DualSense/DS4/Switch identity can't carry the power key (the Sony/Nintendo kernel
        // driver would grab it). Briefly become the power-key device; we're shutting down anyway.
        key_identity = true;
        key_phase = KEY_WANT_PRESS;
        return true;
    }
    if (!usb_allowed()) return false;   // BC-250 USB not powered
    if (tud_suspended()) {        // host asleep: wake it, the key goes out after resume
        tud_remote_wakeup();
        key_phase = KEY_WANT_PRESS;
        return true;
    }
    if (!tud_mounted()) return false;
    key_phase = KEY_WANT_PRESS;
    return true;
}

static void power_key_task(uint32_t t) {
    static const uint8_t press = 0x01, release = 0x00;   // 1 = System Power Down
    if (!usb_has_power_key()) return;
    switch (key_phase) {
        case KEY_WANT_PRESS:
            // give systemd-logind a moment to pick up a freshly (re)plugged key device
            if (tud_mounted() && t - usb_mounted_at >= 1500 && tud_hid_ready() && tud_hid_report(0, &press, 1)) {
                key_phase = KEY_HELD;
                key_pressed_at = t;
                printf("bc250: power key sent\n");
            }
            break;
        case KEY_HELD:
            if (t - key_pressed_at >= POWER_KEY_PRESS_MS) key_phase = KEY_WANT_RELEASE;
            break;
        case KEY_WANT_RELEASE:
            if (tud_hid_ready() && tud_hid_report(0, &release, 1)) key_phase = KEY_IDLE;
            else if (!tud_mounted()) key_phase = KEY_IDLE;
            break;
        default:
            break;
    }
}

// ------------------------------------------------------------------ LED (PWM)
static void led_init(void) {
    gpio_set_function(PIN_LED, GPIO_FUNC_PWM);
    uint slice = pwm_gpio_to_slice_num(PIN_LED);
    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_wrap(&cfg, 255);
    pwm_config_set_clkdiv(&cfg, 16.f);   // ~36 kHz at 150 MHz: no visible flicker
    pwm_init(slice, &cfg, true);
    pwm_set_gpio_level(PIN_LED, 0);
}

static void led_task(uint32_t t) {
    uint8_t level;
    bool blink_fast = (state == ST_STOPPING) || (state == ST_RUNNING && tpm_low_since);
    if (state == ST_OFF)
        level = LED_BRIGHTNESS_STANDBY;
    else if (blink_fast)
        level = ((t / LED_BLINK_STOPPING_MS) & 1) ? 0 : LED_BRIGHTNESS_ON;
    else if (state == ST_STARTING)
        level = ((t / LED_BLINK_STARTING_MS) & 1) ? 0 : LED_BRIGHTNESS_ON;
    else
        level = LED_BRIGHTNESS_ON;
    // Low battery (v8): short dark blink every 3 s while the console is on
    if (state == ST_RUNNING && battery_low_any && (t % 3000) < 150) level = 0;
    // Pairing window: double blink (two short flashes every second)
    if (bt_pairing_active()) {
        uint32_t ph = t % 1000;
        level = (ph < LED_BLINK_PAIRING_MS || (ph >= 2 * LED_BLINK_PAIRING_MS && ph < 3 * LED_BLINK_PAIRING_MS))
                    ? LED_BRIGHTNESS_ON : 0;
    }
    // Hold feedback: from 2 s the LED double-blinks fast ("release now = pairing"); with the PSU on,
    // past PAIR_HOLD_MAX_MS it goes dark ("keep holding = force off")
    if (btn_db.stable && !btn_long_fired) {
        uint32_t held = t - btn_down_at;
        if (state != ST_OFF && held >= PAIR_HOLD_MAX_MS) level = 0;
        else if (state == ST_OFF && held >= WIFI_HOLD_MS) level = LED_BRIGHTNESS_ON;   // "release = Wi-Fi on"
        else if (held >= PAIR_HOLD_MS) level = ((t / 60) & 1) ? LED_BRIGHTNESS_ON : 0;
    }
    pwm_set_gpio_level(PIN_LED, level);
}

// ------------------------------------------------------------------ power actions
static void power_on(const char* why) {
    printf("bc250: PSU on (%s)\n", why);
    psu(true);
    psu_was_ever_on = true;
    tpm_low_since = 0;
    set_state(ST_STARTING);
}

static void power_off(const char* why) {
    printf("bc250: PSU off (%s)\n", why);
    psu(false);
    psu_off_at = now_ms();
    tpm_low_since = 0;
    key_phase = KEY_IDLE;
    key_identity = false;
    bt_disconnect_all();          // controllers go to sleep; their Home button wakes us later
    set_state(ST_OFF);
}

static bool bt_wake_allowed(uint32_t t) {
    return !psu_was_ever_on || t - psu_off_at >= BT_WAKE_COOLDOWN_MS;
}

// ------------------------------------------------------------------ USB callbacks (tud_task context)
// Xbox 360 pad interface `idx` got a rumble command -> remember it for that controller's slot
void xinput_rumble_cb(uint8_t idx, uint8_t strong, uint8_t weak) {
    if (idx >= usb_n || usb_map[idx] < 0) return;
    pad_t* p = &pads[usb_map[idx]];
    if (p->rumble_strong == strong && p->rumble_weak == weak) return;
    p->rumble_strong = strong;
    p->rumble_weak = weak;
    p->rumble_pending = true;
}

void tud_mount_cb(void) {
    usb_mounted_at = now_ms();
    for (int i = 0; i < MAX_PADS; i++) pads[i].xi_dirty = true;   // resend after (re)enumeration
}

// ------------------------------------------------------------------ which device the BC-250 sees
//  no controllers                         -> idle (power key only)
//  all the same kind (DS5 / DS4 / Switch) -> that controller, once per controller
//  anything else                          -> one Xbox 360 pad per controller
static void choose_usb_identity(void) {
    if (key_identity) {   // shutting down from a native identity: become the power-key device
        if (usb_active == USB_P_XBOX) usb_set_target(USB_P_XBOX, usb_active_mask());
        else usb_set_target(USB_P_IDLE, 0);
        return;
    }
    uint8_t mask = 0;
    bool same = true;
    native_kind_t kind = NAT_NONE;
    for (int i = 0; i < MAX_PADS; i++) {
        if (!pads[i].used) continue;
        if (pads[i].fetching) return;    // wait for its feature reports, then decide (avoids a double re-plug)
        if (!mask) kind = pads[i].kind;
        else if (pads[i].kind != kind) same = false;
        mask |= (uint8_t)(1u << i);
    }
    if (!mask) usb_set_target(USB_P_IDLE, 0);
    else if (same && kind == NAT_DS5) usb_set_target(USB_P_DS5, mask);
    else if (same && kind == NAT_DS4) usb_set_target(USB_P_DS4, mask);
    else if (same && kind == NAT_SWITCH) usb_set_target(USB_P_SWITCH, mask);
    else usb_set_target(USB_P_XBOX, mask);
}

// ------------------------------------------------------------------ BC-250 USB power (VBUS)
// The Pico's VBUS pin is the BC-250's USB 5 V. On the Pico 2 W it is read through the CYW43 chip.
static bool cyw43_ok;
static bool vbus_present = true;
static uint32_t vbus_checked_at;

static void vbus_task(uint32_t t) {
    // Only while the PSU is on: in standby USB is off anyway, so leave the CYW43 chip to Bluetooth
    if (!USE_VBUS_SENSE || state == ST_OFF || bt_restarting()) return;
    if (!cyw43_ok || t - vbus_checked_at < VBUS_POLL_MS) return;
    vbus_checked_at = t;
    vbus_present = cyw43_arch_gpio_get(CYW43_WL_GPIO_VBUS_PIN);
}

// ------------------------------------------------------------------ self-restart (PSU off only)
// Reason codes kept in a watchdog scratch register (survives the restart, cleared at power-on)
#define RESTART_MAGIC   0xBC250000u
enum { RS_NONE, RS_BT_DEAD, RS_IDLE };
static const char* const restart_name[] = {"", "Bluetooth could not be repaired", "scheduled idle restart"};
static bool bt_dead;

static void restart_pico(int reason) {
    printf("bc250: restarting the Pico (%s) - PSU stays off\n", restart_name[reason]);
    watchdog_hw->scratch[0] = RESTART_MAGIC | (uint32_t)reason;
    watchdog_hw->scratch[1]++;                                  // restarts since power-on
    stdio_flush();
    watchdog_reboot(0, 0, 10);
    while (true) tight_loop_contents();
}

static bool any_pad_connected(void) {
    for (int i = 0; i < MAX_PADS; i++)
        if (pads[i].used) return true;
    return false;
}

// Called outside the lock, every loop
static void restart_task(uint32_t t) {
    if (state != ST_OFF || gpio_get(PIN_PSU)) return;           // never with the board powered
    if (btn_db.stable) return;                                  // button pressed right now
    uint32_t off_for = psu_was_ever_on ? t - psu_off_at : t;
    if (off_for < RESTART_QUIET_MS) return;
    if (bt_dead) {
        // Back off if restarting didn't help (radio really broken): then only every 30 min
        if (watchdog_hw->scratch[2] < 3 || off_for >= 30u * 60u * 1000u) {
            watchdog_hw->scratch[2]++;
            restart_pico(RS_BT_DEAD);
        }
        return;
    }
    if (t >= 10u * 60u * 1000u) watchdog_hw->scratch[2] = 0;   // healthy for 10 min: reset back-off
    if (IDLE_RESTART_MS && off_for >= IDLE_RESTART_MS && !any_pad_connected()) restart_pico(RS_IDLE);
}

// ------------------------------------------------------------------ main tick (async_context lock held)
static uint32_t seen_connected, seen_system, seen_activity;
static uint32_t status_logged_at;

static void print_status(void) {
    uint32_t t = now_ms();
    char b[200] = "bt off";
    if (cyw43_ok) bt_health_status(t, b, sizeof(b));
    printf("bc250: status up=%lus state=%s psu=%d usb=%d vbus=%d | %s\n", (unsigned long)(t / 1000),
           state_name[state], gpio_get(PIN_PSU), usb_allowed(), vbus_present, b);
}

// ------------------------------------------------------------------ v8: Wi-Fi requests, status, low battery
static volatile net_req_t net_req;   // set from lwIP callbacks / log port, handled in app_tick

void app_request(net_req_t r) { net_req = r; }

static int battery_pct(uint8_t b) {   // Bluepad32: 1 = empty .. 255 = full, 0 = unknown
    if (!b) return -1;
    int p = ((int)b - 1) * 100 / 250;
    return p > 100 ? 100 : p;
}

void app_get_status(app_status_t* st) {
    st->state = state_name[state];
    st->psu = gpio_get(PIN_PSU);
    st->uptime_s = now_ms() / 1000;
    st->pairing = cyw43_ok && bt_pairing_active();
    st->pads = 0;
    st->battery_min = -1;
    for (int i = 0; i < MAX_PADS; i++) {
        if (!pads[i].used) {
            st->battery[i] = -2;   // empty slot
            continue;
        }
        st->pads++;
        int p = battery_pct(pads[i].battery);
        st->battery[i] = p;
        if (p >= 0 && (st->battery_min < 0 || p < st->battery_min)) st->battery_min = p;
    }
}

static void net_requests(void) {
    net_req_t r = net_req;
    if (r == NET_REQ_NONE) return;
    net_req = NET_REQ_NONE;
    switch (r) {
        case NET_REQ_ON:
            if (state == ST_OFF) power_on("web / Home Assistant");
            break;
        case NET_REQ_SHUTDOWN:
            if (state == ST_RUNNING || state == ST_STOPPING) {
                if (request_power_key()) set_state(ST_STOPPING);
                else printf("bc250: USB not connected - cannot send power key\n");
            }
            break;
        case NET_REQ_FORCE_OFF:
            if (state != ST_OFF) power_off("web / Home Assistant: force off");
            break;
        case NET_REQ_PAIR:
            if (cyw43_ok) bt_pairing_start();
            break;
        default:
            break;
    }
}

static uint32_t battery_warned_at[MAX_PADS];
static uint8_t battery_last[MAX_PADS];

static void battery_task(uint32_t t) {
    static uint32_t checked_at;
    if (t - checked_at < 2000) return;
    checked_at = t;
    bool any = false;
    for (int i = 0; i < MAX_PADS; i++) {
        uint8_t b = pads[i].used ? pads[i].battery : 0;
        if (b != battery_last[i]) {
            battery_last[i] = b;
            net_notify_change();
        }
        bool low = b && b <= LOW_BATTERY_LEVEL;
        if (!low) {
            battery_warned_at[i] = 0;
            continue;
        }
        any = true;
        if (!battery_warned_at[i] || t - battery_warned_at[i] >= LOW_BATTERY_REMIND_MS) {
            battery_warned_at[i] = t ? t : 1;
            printf("bc250: controller %d battery low (%d %%)\n", i + 1, battery_pct(b));
            bt_rumble((uint8_t)i, 200, 200);   // short buzz; it stops by itself
        }
    }
    battery_low_any = any;
}

static void app_tick(uint32_t t) {
    btn_event_t btn = poll_button(t);
    debounce(&tpm_db, gpio_get(PIN_TPM), t, TPM_DEBOUNCE_MS);
    bool tpm = tpm_db.stable;

    bool ev_conn = bt_ev_connected != seen_connected;
    bool ev_sys = bt_ev_system_button != seen_system;
    bool ev_act = bt_ev_activity != seen_activity;
    seen_connected = bt_ev_connected;
    seen_system = bt_ev_system_button;
    seen_activity = bt_ev_activity;

    // Hold 5 s: force off, whatever is happening
    if (btn == BTN_LONG && state != ST_OFF) {
        power_off("button held");
        return;
    }
    // Hold 2 s and release: open the pairing window
    if (btn == BTN_PAIR) {
        if (cyw43_ok) bt_pairing_start();
        return;
    }
    // Console off, hold 8 s and release: Wi-Fi on (v8, every Wi-Fi mode)
    if (btn == BTN_WIFI) {
        if (cyw43_ok) net_wifi_request();
        return;
    }

    // TPM low long enough while the board was up -> it has shut down
    if (state == ST_RUNNING || state == ST_STOPPING) {
        if (tpm) {
            tpm_low_since = 0;
        } else {
            if (!tpm_low_since) tpm_low_since = t ? t : 1;
            if (t - tpm_low_since >= TPM_OFF_DELAY_MS) {
                power_off("TPM low - board shut down");
                return;
            }
        }
    }

    switch (state) {
        case ST_OFF:
            if (btn == BTN_SHORT) {
                power_on("button");
            } else if (ev_conn || ev_sys) {
                if (bt_wake_allowed(t)) {
                    power_on("bluetooth controller");
                } else {
                    printf("bc250: controller reconnected during cooldown - dropping it\n");
                    bt_disconnect_all();
                }
            }
            break;

        case ST_STARTING:
            if (tpm) {
                set_state(ST_RUNNING);
            } else if (BOOT_TIMEOUT_MS && t - state_since >= BOOT_TIMEOUT_MS) {
                power_off("board did not start (no TPM signal)");
            }
            // short press while booting is ignored (hold 5 s to abort)
            break;

        case ST_RUNNING:
            if (btn == BTN_SHORT) {
                if (request_power_key()) set_state(ST_STOPPING);
                else printf("bc250: USB not connected - cannot send power key (hold 5 s to force off)\n");
            }
            break;

        case ST_STOPPING:
            if (btn == BTN_SHORT) request_power_key();   // press again = send again
            if (t - state_since >= SHUTDOWN_WAIT_MS) {   // SteamOS ignored the key - give up
                key_phase = KEY_IDLE;
                key_identity = false;
                set_state(ST_RUNNING);
            }
            break;
    }

    // USB only while the BC-250 is powered: no leak into it, no waiting for a host that is gone
    vbus_task(t);
    usb_allow(state != ST_OFF && (!USE_VBUS_SENSE || vbus_present));

    // Controller pressed something while the BC-250 sleeps (USB suspended) -> wake it
    if (ev_act && state != ST_OFF && usb_allowed() && tud_suspended()) tud_remote_wakeup();

    choose_usb_identity();
    usb_task(t);
    bt_native_task(t);

    // Xbox 360 identity: forward converted reports + rumble
    if (usb_active == USB_P_XBOX && !usb_switching() && tud_mounted()) {
        for (uint8_t k = 0; k < usb_n; k++) {
            pad_t* p = &pads[usb_map[k]];
            if (p->xi_dirty && xinput_send(k, &p->xi)) p->xi_dirty = false;
            if (p->rumble_pending) {
                p->rumble_pending = false;
                bt_rumble((uint8_t)usb_map[k], p->rumble_strong, p->rumble_weak);
            }
        }
    }

    power_key_task(t);
    led_task(t);

    // Bluetooth health + self-repair (Bluepad32/BTstack calls need the lock, so it runs here)
    if (cyw43_ok && !bt_health_task(t) && !bt_dead) {
        bt_dead = true;
        printf("bc250: Bluetooth restarts did not help - Pico will restart %s\n",
               state == ST_OFF ? "now" : "after the board is switched off");
    }
    if (STATUS_LOG_MS && t - status_logged_at >= STATUS_LOG_MS) {
        status_logged_at = t;
        print_status();
    }
    log_task();   // USB serial log port (idle identity only)
    if (cyw43_ok) {
        net_requests();
        battery_task(t);
        net_task(t);
    }
}

// ------------------------------------------------------------------ main
int main(void) {
    // PSU control first: make sure it is (and stays) off
    gpio_init(PIN_PSU);
    gpio_put(PIN_PSU, 0);
    gpio_set_dir(PIN_PSU, GPIO_OUT);

    gpio_init(PIN_BUTTON);
    gpio_set_dir(PIN_BUTTON, GPIO_IN);
    gpio_pull_up(PIN_BUTTON);

    gpio_init(PIN_TPM);
    gpio_set_dir(PIN_TPM, GPIO_IN);
    gpio_pull_down(PIN_TPM);

    led_init();
    stdio_init_all();   // UART0 on GP0/GP1 (debug log only, nothing needs to be connected)
    log_init(watchdog_caused_reboot());   // RAM log, kept across self-restarts
    log_set_status_fn(print_status);
    printf("\nbc250: power controller starting (firmware v8)\n");
    if (watchdog_caused_reboot()) {
        uint32_t r = watchdog_hw->scratch[0];
        if ((r & 0xFFFF0000u) == RESTART_MAGIC && (r & 0xFFFF) <= RS_IDLE)
            printf("bc250: restarted on purpose: %s (restarts since power-on: %lu)\n",
                   restart_name[r & 0xFFFF], (unsigned long)watchdog_hw->scratch[1]);
        else
            printf("bc250: restarted by the watchdog - firmware had stopped\n");
    } else {
        watchdog_hw->scratch[1] = 0;   // real power-on
        watchdog_hw->scratch[2] = 0;
    }
    watchdog_hw->scratch[0] = 0;

    usb_device_init();
    tusb_init();
    tud_disconnect();   // tusb_init() switches the pull-up on; stay off until the PSU is on

    async_context_t* ctx = NULL;
    if (cyw43_arch_init()) {
        printf("bc250: cyw43 init failed - running without Bluetooth\n");
    } else {
        bt_bridge_start();
        ctx = cyw43_arch_async_context();
        cyw43_ok = true;
        settings_load();   // v8: Wi-Fi / MQTT settings from flash
        net_init();
    }

    // Start from the real button state so a press held during power-up is not seen as a click
    uint32_t t0 = now_ms();
    btn_db.stable = btn_db.last_raw = !gpio_get(PIN_BUTTON);
    btn_long_fired = true;
    btn_down_at = t0;
    state_since = t0;

    bool wd_armed = false;
    while (true) {
        tud_task();
        uint32_t t = now_ms();
        if (ctx) async_context_acquire_lock_blocking(ctx);
        app_tick(t);
        if (ctx) async_context_release_lock(ctx);
        restart_task(t);

        // Watchdog only while the PSU is off: if anything hangs, the Pico restarts by itself
        if (state == ST_OFF) {
            if (!wd_armed) { watchdog_enable(WATCHDOG_OFF_MS, true); wd_armed = true; }
            watchdog_update();
        } else if (wd_armed) {
            watchdog_disable();
            wd_armed = false;
        }
        sleep_us(500);
    }
}
