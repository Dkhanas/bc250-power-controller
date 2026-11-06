#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bt_bridge.h"
#include "dhcpd.h"
#include "dnsd.h"
#include "web_assets.h"
#include "lwip/apps/mdns.h"
#if BC250_HA
#include "lwip/apps/mqtt.h"
#endif
#include "lwip/dns.h"
#include "lwip/ip_addr.h"
#include "lwip/tcp.h"
#include "pico/cyw43_arch.h"
#include "pico/unique_id.h"
#include "settings.h"

#if BC250_HA
#define FW_VERSION "v8-ha"   // with Home Assistant (bc250_power_ha.uf2)
#else
#define FW_VERSION "v8"
#endif

// ================================================================== Wi-Fi
// Normal: join the saved network (station). Setup: the Pico's own hotspot "BC250-setup" with a settings page
// at http://192.168.4.1 - when no network is saved yet, or when the saved one has failed for WIFI_FAIL_TO_SETUP_MS
// (then for SETUP_HOTSPOT_MS, after which joining is tried again).
typedef enum { W_OFF, W_CONNECTING, W_UP } wifi_state_t;
static wifi_state_t w = W_OFF;
static uint32_t w_deadline, w_retry_at;
static uint32_t seen_settings = 0xFFFFFFFFu;
static bool sta_enabled, mdns_inited, mdns_on_netif, http_started;
static bool ap_on;                 // setup hotspot running
static uint32_t ap_until;          // 0 = no forced hotspot period
static uint32_t sta_fail_since;    // saved network not reachable since (0 = fine / not trying)
static bool change_pending;
static bool setup_scanned;         // network list taken before opening the hotspot
static uint32_t req_until;         // Wi-Fi requested until (0 = no request)
static bool console_on;            // for WIFI_MODE_WHILE_OFF
static uint32_t console_on_at;

static uint32_t now_ms_net(void) { return to_ms_since_boot(get_absolute_time()); }

const char* net_mode_name(int mode) {
    switch (mode) {
        case WIFI_MODE_ALWAYS: return "always on";
        case WIFI_MODE_WHILE_OFF: return "only while the console is off";
        case WIFI_MODE_ON_REQUEST: return "only when turned on";
        default: return "off";
    }
}

void net_wifi_request(void) {
    req_until = now_ms_net() + WIFI_REQUEST_MS;
    if (!req_until) req_until = 1;
    printf("bc250: wifi: turned on for %lu min%s\n", (unsigned long)(WIFI_REQUEST_MS / 60000),
           settings.ssid[0] ? " (after the last use of the page)" : " - join BC250-setup on your phone");
}

void net_console_on(bool on) {
    if (on && !console_on) console_on_at = now_ms_net();
    console_on = on;
}

static bool req_active(uint32_t t) { return req_until && (int32_t)(t - req_until) < 0; }

// May Wi-Fi run right now? A request turns it on in every mode.
static bool wifi_allowed(uint32_t t) {
    if (req_active(t)) return true;
    switch (settings.wifi_mode) {
        case WIFI_MODE_ALWAYS: return true;
        case WIFI_MODE_WHILE_OFF: return !console_on || t - console_on_at < WIFI_WHILE_OFF_GRACE_MS;
        default: return false;
    }
}
static char id_hex[8];   // last 3 bytes of the board id, e.g. "a1b2c3"

static struct netif* sta_netif(void) { return &cyw43_state.netif[CYW43_ITF_STA]; }
static struct netif* ap_netif(void) { return &cyw43_state.netif[CYW43_ITF_AP]; }

void net_notify_change(void) { change_pending = true; }

void net_radio_was_reset(void) {
    sta_enabled = false;   // the chip lost its Wi-Fi state; the netifs were removed
    mdns_on_netif = false;
    if (ap_on) {
        dhcpd_stop();
        dnsd_stop();
        ap_on = false;      // restarted by wifi_task
    }
    if (w != W_OFF) {
        w = W_CONNECTING;
        w_retry_at = 0;
        w_deadline = 0;
    }
}

static void http_start(void);
static void mqtt_on_wifi(bool up);
static bool scanning;
static void scan_start(uint32_t t);
static void scan_task(uint32_t t);

static void sta_stop(void) {
    if (w == W_UP) mqtt_on_wifi(false);
    if (sta_enabled) {
        cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
        cyw43_arch_disable_sta_mode();
        sta_enabled = false;
        mdns_on_netif = false;
    }
    w = W_OFF;
    w_deadline = 0;
}

static void ap_start(void) {
    sta_stop();
    bool open = SETUP_HOTSPOT_PASSWORD[0] == 0;
    cyw43_arch_enable_ap_mode(SETUP_HOTSPOT_SSID, open ? NULL : SETUP_HOTSPOT_PASSWORD,
                              open ? CYW43_AUTH_OPEN : CYW43_AUTH_WPA2_AES_PSK);
    dhcpd_start(ap_netif());
    dnsd_start(ap_netif());   // captive portal: every name -> 192.168.4.1
    http_start();
    ap_on = true;
    printf("bc250: wifi: setup hotspot on - join '%s'%s, the setup page opens by itself (or http://192.168.4.1)\n",
           SETUP_HOTSPOT_SSID, open ? "" : " (password " SETUP_HOTSPOT_PASSWORD ")");
}

static void ap_stop(void) {
    if (!ap_on) return;
    dhcpd_stop();
    dnsd_stop();
    cyw43_arch_disable_ap_mode();
    ap_on = false;
    printf("bc250: wifi: setup hotspot off\n");
}

static void wifi_start(uint32_t t) {
    if (!sta_enabled) {
        cyw43_arch_enable_sta_mode();
        sta_enabled = true;
        mdns_on_netif = false;
    }
    int auth = settings.pass[0] ? CYW43_AUTH_WPA2_MIXED_PSK : CYW43_AUTH_OPEN;
    int r = cyw43_arch_wifi_connect_async(settings.ssid, settings.pass[0] ? settings.pass : NULL, auth);
    printf("bc250: wifi: connecting to '%s'%s\n", settings.ssid, r ? " (start failed)" : "");
    w = W_CONNECTING;
    w_deadline = t + 30000;
    w_retry_at = t + 30000;
    if (!sta_fail_since) sta_fail_since = t ? t : 1;
}

static void wifi_task(uint32_t t) {
    if (settings_version != seen_settings) {   // new / changed settings
        bool first = seen_settings == 0xFFFFFFFFu;
        seen_settings = settings_version;
        sta_fail_since = 0;
        if (ap_on && settings.ssid[0]) {
            ap_until = t + 3000;   // let the settings page get its answer, then switch to the network
        } else {
            ap_until = 0;
            if (!first && w != W_OFF) sta_stop();
            w_retry_at = t;
        }
    }
    if (bt_restarting()) return;   // the radio chip is being reset

    // ---- Wi-Fi allowed at all right now? (Settings: always / only while console off / only when turned on / off)
    if (!wifi_allowed(t)) {
        if (ap_on || sta_enabled || w != W_OFF) {
            if (ap_on) ap_stop();
            sta_stop();
            printf("bc250: wifi: off (%s)\n", net_mode_name(settings.wifi_mode));
        }
        req_until = 0;
        sta_fail_since = 0;
        ap_until = 0;
        setup_scanned = false;
        return;
    }

    // ---- which mode?
    bool want_ap = !settings.ssid[0] || (ap_until && (int32_t)(t - ap_until) < 0);
    if (settings.ssid[0] && !ap_on && sta_fail_since && t - sta_fail_since >= WIFI_FAIL_TO_SETUP_MS) {
        printf("bc250: wifi: '%s' not reachable for %lu min - setup hotspot for %lu min\n", settings.ssid,
               (unsigned long)(WIFI_FAIL_TO_SETUP_MS / 60000), (unsigned long)(SETUP_HOTSPOT_MS / 60000));
        ap_until = t + SETUP_HOTSPOT_MS;
        want_ap = true;
    }
    if (want_ap) {
        if (!ap_on) {
            // first look around for networks (needs the station interface), so the setup page can list them
            if (!setup_scanned) {
                if (!scanning) {
                    if (!sta_enabled) {
                        cyw43_arch_enable_sta_mode();
                        sta_enabled = true;
                    }
                    scan_start(t);
                    if (!scanning) setup_scanned = true;   // scan didn't start: go on without a list
                }
                scan_task(t);
                if (scanning) return;
                setup_scanned = true;
            }
            ap_start();
        }
        return;
    }
    setup_scanned = false;   // next time the hotspot opens, look again
    if (ap_on) {
        ap_stop();
        ap_until = 0;
        sta_fail_since = 0;
        w_retry_at = t;
    }
    if (!settings.ssid[0]) {
        if (w != W_OFF || sta_enabled) sta_stop();
        sta_fail_since = 0;
        return;
    }

    // ---- station: join the saved network
    int ls = sta_enabled ? cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) : CYW43_LINK_DOWN;
    if (sta_enabled && ls == CYW43_LINK_UP) {
        sta_fail_since = 0;
        if (w != W_UP) {
            w = W_UP;
            printf("bc250: wifi: connected, address %s - open http://bc250.local (or http://%s)\n",
                   ip4addr_ntoa(netif_ip4_addr(sta_netif())), ip4addr_ntoa(netif_ip4_addr(sta_netif())));
            if (!mdns_inited) {
                mdns_resp_init();
                mdns_inited = true;
            }
            if (!mdns_on_netif) {
                mdns_resp_add_netif(sta_netif(), "bc250");
                mdns_resp_add_service(sta_netif(), "bc250", "_http", DNSSD_PROTO_TCP, 80, NULL, NULL);
                mdns_on_netif = true;
            }
            http_start();
            mqtt_on_wifi(true);
            net_notify_change();
        }
        return;
    }
    if (w == W_UP) {
        printf("bc250: wifi: connection lost (status %d) - reconnecting\n", ls);
        mqtt_on_wifi(false);
        w = W_CONNECTING;
        w_retry_at = t + 5000;
        w_deadline = 0;
        sta_fail_since = t ? t : 1;
        return;
    }
    // connecting, failed or not started yet
    bool failed = (ls == CYW43_LINK_FAIL || ls == CYW43_LINK_NONET || ls == CYW43_LINK_BADAUTH);
    if (failed && w_deadline) {
        printf("bc250: wifi: failed (%s) - retrying in 30 s\n",
               ls == CYW43_LINK_BADAUTH ? "wrong password" : ls == CYW43_LINK_NONET ? "network not found" : "error");
        w_deadline = 0;
        w_retry_at = t + 30000;
    } else if (w_deadline && (int32_t)(t - w_deadline) >= 0) {
        printf("bc250: wifi: no connection after 30 s - retrying\n");
        w_deadline = 0;
        w_retry_at = t + 5000;
    }
    if (!w_deadline && (int32_t)(t - w_retry_at) >= 0) wifi_start(t);
}

// ================================================================== JSON helpers + status
// JSON-escape s into out (always terminated, quotes included)
static int json_str(char* out, int n, const char* s) {
    int k = 0;
    if (k < n) out[k++] = '"';
    for (; *s && k < n - 3; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            out[k++] = '\\';
            out[k++] = (char)c;
        } else if (c >= 0x20) {
            out[k++] = (char)c;
        }
    }
    if (k < n) out[k++] = '"';
    out[k < n ? k : n - 1] = 0;
    return k;
}

static int status_json(char* b, int n) {
    app_status_t s;
    app_get_status(&s);
    int k = snprintf(b, (size_t)n,
                     "{\"power\":\"%s\",\"state\":\"%s\",\"uptime\":%lu,\"pairing\":%s,\"controllers\":%d,\"battery\":",
                     s.psu ? "ON" : "OFF", s.state, (unsigned long)s.uptime_s, s.pairing ? "true" : "false", s.pads);
    if (s.battery_min >= 0) k += snprintf(b + k, (size_t)(n - k), "%d", s.battery_min);
    else k += snprintf(b + k, (size_t)(n - k), "null");

    // paired controllers (v8)
    static bt_ctrl_info_t c[8];
    int nc = bt_controllers(c, 8);
    k += snprintf(b + k, (size_t)(n - k), ",\"ctrls\":[");
    for (int i = 0; i < nc && k < n - 120; i++) {
        char nm[60];
        json_str(nm, sizeof(nm), c[i].name);
        k += snprintf(b + k, (size_t)(n - k), "%s{\"addr\":\"%s\",\"name\":%s,\"conn\":%s,\"bat\":%d}", i ? "," : "",
                      c[i].addr, nm, c[i].connected ? "true" : "false", c[i].battery);
    }
    bt_pair_info_t pi;
    bt_pairing_info(&pi);
    char pn[60];
    json_str(pn, sizeof(pn), pi.name);
    k += snprintf(b + k, (size_t)(n - k),
                  "],\"pair\":{\"active\":%s,\"left\":%d,\"seq\":%lu,\"result\":\"%s\",\"name\":%s},\"setup\":%s,"
                  "\"wifi\":{\"mode\":%u,\"off_in\":%ld},\"version\":\"%s\"}",
                  pi.active ? "true" : "false", pi.left_s, (unsigned long)pi.seq, pi.result, pn, ap_on ? "true" : "false",
                  settings.wifi_mode,
                  (settings.wifi_mode != WIFI_MODE_ALWAYS && req_active(now_ms_net()))
                      ? (long)((req_until - now_ms_net()) / 1000) : -1L,
                  FW_VERSION);
    return k;
}

// ================================================================== Wi-Fi network list (scan)
#define NET_MAX 16
typedef struct {
    char ssid[33];
    int16_t rssi;
    bool lock;
} wnet_t;
static wnet_t nets[NET_MAX];
static int net_n;
static uint32_t scan_started;

static int scan_cb(void* env, const cyw43_ev_scan_result_t* r) {
    (void)env;
    if (!r || !r->ssid_len || r->ssid_len > 32) return 0;   // hidden network
    char s[33];
    memcpy(s, r->ssid, r->ssid_len);
    s[r->ssid_len] = 0;
    for (int i = 0; i < net_n; i++) {
        if (strcmp(nets[i].ssid, s) == 0) {   // same network seen on another access point / channel
            if (r->rssi > nets[i].rssi) nets[i].rssi = r->rssi;
            return 0;
        }
    }
    int slot = net_n < NET_MAX ? net_n++ : -1;
    if (slot < 0) {   // full: replace the weakest if this one is stronger
        slot = 0;
        for (int i = 1; i < NET_MAX; i++)
            if (nets[i].rssi < nets[slot].rssi) slot = i;
        if (r->rssi <= nets[slot].rssi) return 0;
    }
    snprintf(nets[slot].ssid, sizeof(nets[slot].ssid), "%s", s);
    nets[slot].rssi = r->rssi;
    nets[slot].lock = r->auth_mode != 0;
    return 0;
}

// Start a scan (station interface must be up). Results arrive in scan_cb.
static void scan_start(uint32_t t) {
    if (scanning) return;
    cyw43_wifi_scan_options_t o;
    memset(&o, 0, sizeof(o));
    net_n = 0;
    if (cyw43_wifi_scan(&cyw43_state, &o, NULL, scan_cb) == 0) {
        scanning = true;
        scan_started = t;
    }
}

static void scan_task(uint32_t t) {
    if (scanning && (!cyw43_wifi_scan_active(&cyw43_state) || t - scan_started > 10000)) {
        scanning = false;
        printf("bc250: wifi: %d networks found\n", net_n);
    }
}

static int networks_json(char* b, int n) {
    // strongest first
    int order[NET_MAX];
    for (int i = 0; i < net_n; i++) order[i] = i;
    for (int i = 0; i < net_n; i++)
        for (int j = i + 1; j < net_n; j++)
            if (nets[order[j]].rssi > nets[order[i]].rssi) {
                int x = order[i];
                order[i] = order[j];
                order[j] = x;
            }
    int k = snprintf(b, (size_t)n, "{\"scanning\":%s,\"list\":[", scanning ? "true" : "false");
    for (int i = 0; i < net_n && k < n - 90; i++) {
        char s[72];
        json_str(s, sizeof(s), nets[order[i]].ssid);
        k += snprintf(b + k, (size_t)(n - k), "%s{\"ssid\":%s,\"rssi\":%d,\"lock\":%s}", i ? "," : "", s,
                      nets[order[i]].rssi, nets[order[i]].lock ? "true" : "false");
    }
    k += snprintf(b + k, (size_t)(n - k), "]}");
    return k;
}

static int settings_json(char* b, int n) {
    char ss[80], mh[140], mu[70];
    json_str(ss, sizeof(ss), settings.ssid);
    json_str(mh, sizeof(mh), settings.mqtt_host);
    json_str(mu, sizeof(mu), settings.mqtt_user);
    return snprintf(b, (size_t)n,
                    "{\"setup\":%s,\"ssid\":%s,\"mqtt_host\":%s,\"mqtt_port\":%u,\"mqtt_user\":%s,\"mqtt_pass\":%s,\"pin\":%s,"
                    "\"wifi_mode\":%u,\"ha\":%s}",
                    ap_on ? "true" : "false", ss, mh, settings.mqtt_port, mu, settings.mqtt_pass[0] ? "true" : "false",
                    settings.pin[0] ? "true" : "false", settings.wifi_mode, BC250_HA ? "true" : "false");
}

// Copy the url-decoded value of `key` from a x-www-form-urlencoded body. Returns false if the key is absent.
static bool form_get(const char* body, const char* key, char* out, size_t n) {
    size_t kl = strlen(key);
    const char* p = body;
    while (p && *p) {
        if (strncmp(p, key, kl) == 0 && p[kl] == '=') {
            p += kl + 1;
            size_t i = 0;
            while (*p && *p != '&') {
                char c = *p++;
                if (c == '+') c = ' ';
                else if (c == '%' && p[0] && p[1]) {
                    char hx[3] = {p[0], p[1], 0};
                    c = (char)strtol(hx, NULL, 16);
                    p += 2;
                }
                if (i + 1 < n) out[i++] = c;
            }
            out[i] = 0;
            return true;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return false;
}

// Apply a settings form. Empty password fields keep the saved password (unless the network name changed).
static void settings_apply(const char* body, char* msg, int mn) {
    char v[80];
    bool wifi_changed = false;
    if (form_get(body, "ssid", v, sizeof(v)) && v[0]) {
        char pw[64] = "";
        bool has_pw = form_get(body, "pass", pw, sizeof(pw)) && pw[0];
        if (strcmp(v, settings.ssid) != 0) {
            snprintf(settings.ssid, sizeof(settings.ssid), "%s", v);
            snprintf(settings.pass, sizeof(settings.pass), "%s", has_pw ? pw : "");
            wifi_changed = true;
        } else if (has_pw) {
            snprintf(settings.pass, sizeof(settings.pass), "%s", pw);
            wifi_changed = true;
        }
    }
    if (form_get(body, "mqtt_host", v, sizeof(v))) {
        snprintf(settings.mqtt_host, sizeof(settings.mqtt_host), "%s", v);
        if (form_get(body, "mqtt_port", v, sizeof(v))) {
            int port = atoi(v);
            settings.mqtt_port = (port > 0 && port < 65536) ? (uint16_t)port : 1883;
        }
        if (form_get(body, "mqtt_user", v, sizeof(v))) {
            snprintf(settings.mqtt_user, sizeof(settings.mqtt_user), "%s", v);
            if (!v[0]) settings.mqtt_pass[0] = 0;
        }
        if (form_get(body, "mqtt_pass", v, sizeof(v)) && v[0])
            snprintf(settings.mqtt_pass, sizeof(settings.mqtt_pass), "%s", v);
    }
    if (form_get(body, "wifi_mode", v, sizeof(v)) && v[0] >= '0' && v[0] <= '3' && !v[1]) settings.wifi_mode = (uint8_t)(v[0] - '0');
    if (form_get(body, "pin_clear", v, sizeof(v)) && v[0] == '1') settings.pin[0] = 0;
    else if (form_get(body, "pin", v, sizeof(v)) && v[0]) snprintf(settings.pin, sizeof(settings.pin), "%s", v);
    settings_save();
    printf("bc250: web: settings saved%s\n", wifi_changed ? " (Wi-Fi changed)" : "");
    if (ap_on && settings.ssid[0])
        snprintf(msg, (size_t)mn, "Saved. The BC-250 now joins your network: reconnect your phone to it and open http://bc250.local");
    else if (wifi_changed)
        snprintf(msg, (size_t)mn, "Saved. The BC-250 reconnects to the new network; this page stops answering until you're on it too.");
    else
        snprintf(msg, (size_t)mn, "Saved.");
}

// ================================================================== tiny HTTP server (lwIP raw API)
#define HTTP_CONNS 3
typedef struct {
    struct tcp_pcb* pcb;
    char req[1280];
    uint16_t len;
    bool done;
    uint8_t idle;
} hconn_t;
static hconn_t hc[HTTP_CONNS];

static void h_close(hconn_t* c) {
    if (!c->pcb) return;
    tcp_arg(c->pcb, NULL);
    tcp_recv(c->pcb, NULL);
    tcp_sent(c->pcb, NULL);
    tcp_poll(c->pcb, NULL, 0);
    tcp_err(c->pcb, NULL);
    if (tcp_close(c->pcb) != ERR_OK) tcp_abort(c->pcb);
    c->pcb = NULL;
}

static void h_send_hdr(hconn_t* c, int code, const char* type, int blen, const char* extra) {
    char hdr[220];
    const char* msg = code == 200   ? "OK"
                      : code == 302 ? "Found"
                      : code == 403 ? "Forbidden"
                      : code == 405 ? "Method Not Allowed"
                                    : "Not Found";
    int hl = snprintf(hdr, sizeof(hdr), "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %d\r\n%sConnection: close\r\n\r\n",
                      code, msg, type, blen, extra ? extra : "Cache-Control: no-store\r\n");
    tcp_write(c->pcb, hdr, (u16_t)hl, TCP_WRITE_FLAG_COPY | (blen ? TCP_WRITE_FLAG_MORE : 0));
}

// Dynamic body (copied)
static void h_send(hconn_t* c, int code, const char* type, const char* body, int blen) {
    h_send_hdr(c, code, type, blen, NULL);
    if (blen) tcp_write(c->pcb, body, (u16_t)blen, TCP_WRITE_FLAG_COPY);
    tcp_output(c->pcb);
    c->done = true;
}

// Embedded file: sent straight from flash (no copy, so several files can load at once)
static void h_send_asset(hconn_t* c, const web_asset_t* a) {
    h_send_hdr(c, 200, a->type, (int)a->len, "Cache-Control: max-age=60\r\n");
    tcp_write(c->pcb, a->data, (u16_t)a->len, 0);
    tcp_output(c->pcb);
    c->done = true;
}

static void h_redirect(hconn_t* c, const char* to) {
    char extra[96];
    snprintf(extra, sizeof(extra), "Location: %s\r\nCache-Control: no-store\r\n", to);
    h_send_hdr(c, 302, "text/plain", 0, extra);
    tcp_output(c->pcb);
    c->done = true;
}

static const web_asset_t* asset(const char* path) {
    for (unsigned i = 0; i < WEB_ASSET_COUNT; i++)
        if (strcmp(web_assets[i].path, path) == 0) return &web_assets[i];
    return NULL;
}

static bool pin_ok(const char* req) {
    if (!settings.pin[0]) return true;
    const char* h = strstr(req, "\r\nX-PIN:");
    if (!h) h = strstr(req, "\r\nx-pin:");
    if (!h) return false;
    h += 8;
    while (*h == ' ') h++;
    size_t n = strlen(settings.pin);
    return strncmp(h, settings.pin, n) == 0 && (h[n] == '\r' || h[n] == ' ');
}

// Host header is our hotspot address (anything else on the hotspot = a captive-portal check or a typed name)
static bool host_is_me(const char* req) {
    const char* h = strstr(req, "\r\nHost:");
    if (!h) h = strstr(req, "\r\nhost:");
    if (!h) return true;
    h += 7;
    while (*h == ' ') h++;
    return strncmp(h, "192.168.4.1", 11) == 0 && (h[11] == '\r' || h[11] == ':');
}

static char resp[1600];   // response buffer (lwIP callbacks run one at a time; keeps it off the small stack)

static void h_handle(hconn_t* c) {
    if (req_active(now_ms_net())) req_until = now_ms_net() + WIFI_REQUEST_MS;   // in use: keep it on
    char method[8] = "", path[48] = "";
    sscanf(c->req, "%7s %47s", method, path);
    char* q = strchr(path, '?');
    if (q) *q = 0;
    bool get = strcmp(method, "GET") == 0, post = strcmp(method, "POST") == 0;
    const char* body = strstr(c->req, "\r\n\r\n");
    body = body ? body + 4 : "";

    // Setup hotspot: everything that isn't for us goes to the setup page (phones then open it by themselves)
    if (ap_on && !host_is_me(c->req)) {
        h_redirect(c, "http://192.168.4.1/");
        return;
    }

    // ---- pages and files
    if (get && strncmp(path, "/api/", 5) != 0) {
        const web_asset_t* a = NULL;
        if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) a = asset(ap_on ? "/setup.html" : "/index.html");
        else if (strcmp(path, "/settings") == 0) a = asset(ap_on ? "/setup.html" : "/settings.html");
        else if (strcmp(path, "/setup") == 0) a = asset("/setup.html");
        else a = asset(path);
        if (a) h_send_asset(c, a);
        else if (ap_on) h_redirect(c, "http://192.168.4.1/");
        else h_send(c, 404, "text/plain", "not found", 9);
        return;
    }

    // ---- read-only API (no PIN)
    if (get && strcmp(path, "/api/status") == 0) {
        h_send(c, 200, "application/json", resp, status_json(resp, sizeof(resp)));
        return;
    }
    if (get && strcmp(path, "/api/networks") == 0) {
        h_send(c, 200, "application/json", resp, networks_json(resp, sizeof(resp)));
        return;
    }

    // ---- everything else needs POST (settings GET too) and the PIN, if one is set
    bool settings_get = get && strcmp(path, "/api/settings") == 0;
    if (!post && !settings_get) {
        h_send(c, 405, "text/plain", "use POST", 8);
        return;
    }
    // In setup mode the network form works without the PIN (nothing to protect yet), the rest doesn't
    bool open_ok = ap_on && strcmp(path, "/api/settings") == 0 && !settings.ssid[0];
    if (!open_ok && !pin_ok(c->req)) {
        h_send(c, 403, "text/plain", "PIN required", 12);
        return;
    }
    if (settings_get) {
        h_send(c, 200, "application/json", resp, settings_json(resp, sizeof(resp)));
        return;
    }
    if (strcmp(path, "/api/settings") == 0) {
        char m[200], js[220];
        settings_apply(body, m, sizeof(m));
        json_str(js, sizeof(js), m);
        h_send(c, 200, "application/json", resp, snprintf(resp, sizeof(resp), "{\"ok\":true,\"msg\":%s}", js));
        return;
    }
    if (strcmp(path, "/api/networks/scan") == 0) {
        if (!ap_on && sta_enabled) scan_start(to_ms_since_boot(get_absolute_time()));
        h_send(c, 200, "application/json", "{\"ok\":true}", 11);
        return;
    }
    if (strcmp(path, "/api/forget") == 0) {
        char addr[24];
        bool ok = form_get(body, "addr", addr, sizeof(addr)) && bt_forget(addr);
        if (ok) h_send(c, 200, "application/json", "{\"ok\":true}", 11);
        else h_send(c, 404, "text/plain", "unknown controller", 18);
        return;
    }
    struct {
        const char* p;
        net_req_t r;
    } acts[] = {{"/api/on", NET_REQ_ON}, {"/api/shutdown", NET_REQ_SHUTDOWN}, {"/api/off", NET_REQ_FORCE_OFF}, {"/api/pair", NET_REQ_PAIR}};
    for (unsigned i = 0; i < sizeof(acts) / sizeof(acts[0]); i++) {
        if (strcmp(path, acts[i].p) != 0) continue;
        printf("bc250: web: %s\n", acts[i].p + 5);
        app_request(acts[i].r);
        h_send(c, 200, "application/json", "{\"ok\":true}", 11);
        return;
    }
    h_send(c, 404, "text/plain", "not found", 9);
}

static err_t h_sent(void* arg, struct tcp_pcb* pcb, u16_t len) {
    (void)len;
    hconn_t* c = arg;
    if (c && c->done && tcp_sndqueuelen(pcb) == 0) h_close(c);
    return ERR_OK;
}

static err_t h_recv(void* arg, struct tcp_pcb* pcb, struct pbuf* p, err_t err) {
    hconn_t* c = arg;
    if (!c) {
        if (p) pbuf_free(p);
        return ERR_OK;
    }
    if (!p || err != ERR_OK) {   // closed by the other side
        if (p) pbuf_free(p);
        h_close(c);
        return ERR_OK;
    }
    if (!c->done) {
        uint16_t room = (uint16_t)(sizeof(c->req) - 1 - c->len);
        uint16_t n = p->tot_len < room ? p->tot_len : room;
        pbuf_copy_partial(p, c->req + c->len, n, 0);
        c->len += n;
        c->req[c->len] = 0;
        c->idle = 0;
        const char* end = strstr(c->req, "\r\n\r\n");
        bool complete = c->len >= sizeof(c->req) - 1;
        if (end && !complete) {
            const char* cl = strstr(c->req, "\r\nContent-Length:");
            if (!cl) cl = strstr(c->req, "\r\ncontent-length:");
            int need = cl ? atoi(cl + 17) : 0;
            complete = (int)(c->len - (uint16_t)(end + 4 - c->req)) >= need;
        }
        if (complete) h_handle(c);
    }
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static err_t h_poll(void* arg, struct tcp_pcb* pcb) {
    (void)pcb;
    hconn_t* c = arg;
    if (c && ++c->idle >= 10) h_close(c);   // ~5 s without progress
    return ERR_OK;
}

static void h_err(void* arg, err_t err) {
    (void)err;
    hconn_t* c = arg;
    if (c) c->pcb = NULL;   // pcb already freed by lwIP
}

static err_t h_accept(void* arg, struct tcp_pcb* pcb, err_t err) {
    (void)arg;
    if (err != ERR_OK || !pcb) return ERR_VAL;
    hconn_t* c = NULL;
    for (int i = 0; i < HTTP_CONNS; i++)
        if (!hc[i].pcb) c = &hc[i];
    if (!c) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    memset(c, 0, sizeof(*c));
    c->pcb = pcb;
    tcp_arg(pcb, c);
    tcp_recv(pcb, h_recv);
    tcp_sent(pcb, h_sent);
    tcp_poll(pcb, h_poll, 1);
    tcp_err(pcb, h_err);
    return ERR_OK;
}

static void http_start(void) {
    if (http_started) return;
    struct tcp_pcb* pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (!pcb) return;
    if (tcp_bind(pcb, IP_ANY_TYPE, 80) != ERR_OK) {
        tcp_close(pcb);
        return;
    }
    struct tcp_pcb* l = tcp_listen_with_backlog(pcb, 2);
    if (!l) {
        tcp_close(pcb);
        return;
    }
    tcp_accept(l, h_accept);
    http_started = true;
}

// ================================================================== MQTT + Home Assistant discovery
// Only in the Home Assistant variant (bc250_power_ha.uf2, BC250_HA=1).
#if BC250_HA
static mqtt_client_t* mq;
static bool mq_want, mq_busy_dns, mq_pub_busy;
static uint32_t mq_retry_at, mq_state_at;
static int mq_step;           // next item of the announce sequence (-1 = done)
static ip_addr_t mq_ip;
static char base[24];         // "bc250/a1b2c3"
static char cid[16];          // "bc250_a1b2c3"
static char in_topic[64];

#define MQ_ITEMS 8   // availability + 6 discovery configs + state

static void mq_pub_done(void* arg, err_t result) {
    (void)arg;
    (void)result;
    mq_pub_busy = false;
}

static bool mq_pub(const char* topic, const char* payload, bool retain) {
    err_t e = mqtt_publish(mq, topic, payload, (u16_t)strlen(payload), 1, retain ? 1 : 0, mq_pub_done, NULL);
    if (e == ERR_OK) mq_pub_busy = true;
    return e == ERR_OK;
}

static void dev_json(char* b, int n) {
    snprintf(b, (size_t)n,
             "\"avty_t\":\"%s/availability\",\"dev\":{\"ids\":[\"%s\"],\"name\":\"BC-250\",\"mf\":\"DIY\","
             "\"mdl\":\"BC-250 power controller\",\"sw\":\"%s\"}",
             base, cid, FW_VERSION);
}

// Builds item `step` of the announce sequence into topic/payload. Returns retain flag.
static bool mq_item(int step, char* topic, int tn, char* p, int pn) {
    static char dev[240];
    dev_json(dev, sizeof(dev));
    switch (step) {
        case 0:
            snprintf(topic, (size_t)tn, "%s/availability", base);
            snprintf(p, (size_t)pn, "online");
            return true;
        case 1:
            snprintf(topic, (size_t)tn, "homeassistant/switch/%s/power/config", cid);
            snprintf(p, (size_t)pn,
                     "{\"name\":\"Power\",\"uniq_id\":\"%s_power\",\"cmd_t\":\"%s/cmd/power\",\"stat_t\":\"%s/state\","
                     "\"val_tpl\":\"{{ value_json.power }}\",\"pl_on\":\"ON\",\"pl_off\":\"OFF\",\"ic\":\"mdi:gamepad-variant\",%s}",
                     cid, base, base, dev);
            return true;
        case 2:
            snprintf(topic, (size_t)tn, "homeassistant/button/%s/force_off/config", cid);
            snprintf(p, (size_t)pn,
                     "{\"name\":\"Force off\",\"uniq_id\":\"%s_force_off\",\"cmd_t\":\"%s/cmd/force_off\",\"pl_prs\":\"PRESS\","
                     "\"ic\":\"mdi:power-plug-off\",%s}",
                     cid, base, dev);
            return true;
        case 3:
            snprintf(topic, (size_t)tn, "homeassistant/button/%s/pair/config", cid);
            snprintf(p, (size_t)pn,
                     "{\"name\":\"Pair a new controller\",\"uniq_id\":\"%s_pair\",\"cmd_t\":\"%s/cmd/pair\",\"pl_prs\":\"PRESS\","
                     "\"ic\":\"mdi:bluetooth-connect\",%s}",
                     cid, base, dev);
            return true;
        case 4:
            snprintf(topic, (size_t)tn, "homeassistant/sensor/%s/state/config", cid);
            snprintf(p, (size_t)pn,
                     "{\"name\":\"State\",\"uniq_id\":\"%s_state\",\"stat_t\":\"%s/state\",\"val_tpl\":\"{{ value_json.state }}\","
                     "\"ic\":\"mdi:information-outline\",%s}",
                     cid, base, dev);
            return true;
        case 5:
            snprintf(topic, (size_t)tn, "homeassistant/sensor/%s/controllers/config", cid);
            snprintf(p, (size_t)pn,
                     "{\"name\":\"Controllers\",\"uniq_id\":\"%s_controllers\",\"stat_t\":\"%s/state\","
                     "\"val_tpl\":\"{{ value_json.controllers }}\",\"ic\":\"mdi:controller\",%s}",
                     cid, base, dev);
            return true;
        case 6:
            snprintf(topic, (size_t)tn, "homeassistant/sensor/%s/battery/config", cid);
            snprintf(p, (size_t)pn,
                     "{\"name\":\"Controller battery\",\"uniq_id\":\"%s_battery\",\"stat_t\":\"%s/state\","
                     "\"val_tpl\":\"{{ value_json.battery }}\",\"dev_cla\":\"battery\",\"unit_of_meas\":\"%%\",%s}",
                     cid, base, dev);
            return true;
        default:
            snprintf(topic, (size_t)tn, "%s/state", base);
            status_json(p, pn);
            return true;
    }
}

static void mq_incoming_topic(void* arg, const char* topic, u32_t tot_len) {
    (void)arg;
    (void)tot_len;
    snprintf(in_topic, sizeof(in_topic), "%s", topic);
}

static void mq_incoming_data(void* arg, const u8_t* data, u16_t len, u8_t flags) {
    (void)arg;
    if (!(flags & MQTT_DATA_FLAG_LAST)) return;
    char v[16];
    size_t n = len < sizeof(v) - 1 ? len : sizeof(v) - 1;
    memcpy(v, data, n);
    v[n] = 0;
    const char* cmd = strrchr(in_topic, '/');
    if (!cmd) return;
    cmd++;
    if (strcmp(cmd, "power") == 0) {
        if (strcmp(v, "ON") == 0) app_request(NET_REQ_ON);
        else if (strcmp(v, "OFF") == 0) app_request(NET_REQ_SHUTDOWN);
        else return;
    } else if (strcmp(cmd, "force_off") == 0) {
        app_request(NET_REQ_FORCE_OFF);
    } else if (strcmp(cmd, "pair") == 0) {
        app_request(NET_REQ_PAIR);
    } else {
        return;
    }
    printf("bc250: mqtt: %s %s\n", cmd, v);
}

static void mq_connected(mqtt_client_t* client, void* arg, mqtt_connection_status_t status) {
    (void)arg;
    if (status == MQTT_CONNECT_ACCEPTED) {
        printf("bc250: mqtt: connected to %s - announcing to Home Assistant\n", settings.mqtt_host);
        char t[48];
        snprintf(t, sizeof(t), "%s/cmd/#", base);
        mqtt_subscribe(client, t, 1, NULL, NULL);
        mq_step = 0;            // announce sequence starts on the next tick
        mq_pub_busy = false;
    } else {
        printf("bc250: mqtt: connection %s (%d) - retrying in 30 s\n",
               status == MQTT_CONNECT_REFUSED_NOT_AUTHORIZED_ || status == MQTT_CONNECT_REFUSED_USERNAME_PASS ? "refused: login"
                                                                                                          : "lost/failed",
               (int)status);
        mq_step = -1;
        mq_retry_at = to_ms_since_boot(get_absolute_time()) + 30000;
    }
}

static void mq_connect_ip(void) {
    struct mqtt_connect_client_info_t ci;
    memset(&ci, 0, sizeof(ci));
    static char will_topic[40];
    snprintf(will_topic, sizeof(will_topic), "%s/availability", base);
    ci.client_id = cid;
    ci.client_user = settings.mqtt_user[0] ? settings.mqtt_user : NULL;
    ci.client_pass = settings.mqtt_pass[0] ? settings.mqtt_pass : NULL;
    ci.keep_alive = 60;
    ci.will_topic = will_topic;
    ci.will_msg = "offline";
    ci.will_qos = 1;
    ci.will_retain = 1;
    err_t e = mqtt_client_connect(mq, &mq_ip, settings.mqtt_port, mq_connected, NULL, &ci);
    if (e != ERR_OK) {
        printf("bc250: mqtt: connect failed (%d) - retrying in 30 s\n", (int)e);
        mq_retry_at = to_ms_since_boot(get_absolute_time()) + 30000;
    }
}

static void mq_dns_found(const char* name, const ip_addr_t* ip, void* arg) {
    (void)arg;
    mq_busy_dns = false;
    if (!ip) {
        printf("bc250: mqtt: can't find '%s' - retrying in 30 s\n", name);
        mq_retry_at = to_ms_since_boot(get_absolute_time()) + 30000;
        return;
    }
    mq_ip = *ip;
    mq_connect_ip();
}

static void mqtt_on_wifi(bool up) {
    mq_want = up;
    if (!up && mq && mqtt_client_is_connected(mq)) mqtt_disconnect(mq);
    mq_retry_at = 0;
    mq_step = -1;
}

static void mqtt_task(uint32_t t) {
    static uint32_t seen = 0xFFFFFFFFu;
    if (!mq) {
        mq = mqtt_client_new();
        if (!mq) return;
        mqtt_set_inpub_callback(mq, mq_incoming_topic, mq_incoming_data, NULL);
    }
    if (settings_version != seen) {   // broker changed: reconnect with the new settings
        seen = settings_version;
        if (mqtt_client_is_connected(mq)) mqtt_disconnect(mq);
        mq_retry_at = t;
        mq_step = -1;
    }
    if (!mq_want || !settings.mqtt_host[0]) {
        if (mqtt_client_is_connected(mq)) mqtt_disconnect(mq);
        return;
    }
    if (!mqtt_client_is_connected(mq)) {
        if (mq_busy_dns || (int32_t)(t - mq_retry_at) < 0) return;
        mq_retry_at = t + 30000;   // next attempt if this one never answers
        err_t e = dns_gethostbyname(settings.mqtt_host, &mq_ip, mq_dns_found, NULL);
        if (e == ERR_OK) mq_connect_ip();
        else if (e == ERR_INPROGRESS) mq_busy_dns = true;
        return;
    }
    if (mq_pub_busy) return;
    static char topic[80], payload[1600];
    if (mq_step >= 0 && mq_step < MQ_ITEMS) {   // announce sequence, one message at a time
        bool retain = mq_item(mq_step, topic, sizeof(topic), payload, sizeof(payload));
        if (mq_pub(topic, payload, retain)) mq_step++;
        if (mq_step == MQ_ITEMS) {
            mq_step = -1;
            mq_state_at = t;
        }
        return;
    }
    if (change_pending || t - mq_state_at >= 30000) {
        mq_item(MQ_ITEMS - 1, topic, sizeof(topic), payload, sizeof(payload));
        if (mq_pub(topic, payload, true)) {
            change_pending = false;
            mq_state_at = t;
        }
    }
}

#else   // !BC250_HA: no Home Assistant in this firmware
static void mqtt_on_wifi(bool up) { (void)up; }
static void mqtt_task(uint32_t t) {
    (void)t;
    change_pending = false;
}
#endif

// ================================================================== API
void net_init(void) {
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    snprintf(id_hex, sizeof(id_hex), "%02x%02x%02x", id.id[5], id.id[6], id.id[7]);
#if BC250_HA
    snprintf(base, sizeof(base), "bc250/%s", id_hex);
    snprintf(cid, sizeof(cid), "bc250_%s", id_hex);
    mq_step = -1;
#endif
}

void net_task(uint32_t t) {
    wifi_task(t);
    scan_task(t);
    if (w == W_UP) mqtt_task(t);
    else change_pending = false;
}

void net_print_status(void) {
    const char* ws = !wifi_allowed(now_ms_net()) ? "off right now"
                     : ap_on ? "setup hotspot on (join " SETUP_HOTSPOT_SSID ", open http://192.168.4.1)"
                     : !settings.ssid[0] ? "not set up (log port: wifi NETWORK|PASSWORD)"
                     : w == W_UP       ? "connected"
                                       : "connecting";
    printf("bc250: net: wifi mode '%s'%s; ", net_mode_name(settings.wifi_mode), req_active(now_ms_net()) ? " (turned on now)" : "");
    printf("wifi %s", ws);
    if (settings.ssid[0]) printf(" ('%s')", settings.ssid);
    if (w == W_UP) {
        int32_t rssi = 0;
        cyw43_wifi_get_rssi(&cyw43_state, &rssi);
        printf(", address %s, signal %ld dBm, web http://bc250.local", ip4addr_ntoa(netif_ip4_addr(sta_netif())), (long)rssi);
    }
#if BC250_HA
    printf("\nbc250: net: mqtt %s", !settings.mqtt_host[0] ? "off (log port: mqtt HOST[:PORT] [USER|PASSWORD])"
                                    : (mq && mqtt_client_is_connected(mq)) ? "connected"
                                                                           : "not connected");
    if (settings.mqtt_host[0]) printf(" (%s:%u, topics %s/...)", settings.mqtt_host, settings.mqtt_port, base);
#else
    printf("\nbc250: net: no Home Assistant in this firmware (bc250_power_ha.uf2 has it)");
#endif
    printf(", web pin %s\n", settings.pin[0] ? "set" : "off");
}
