// Minimal DHCP server for the setup hotspot (v8). Enough for a phone or laptop to join "BC250-setup":
// DISCOVER -> OFFER, REQUEST -> ACK, up to 8 clients, addresses 192.168.4.16-23, router 192.168.4.1.
#include "dhcpd.h"

#include <stdbool.h>
#include <string.h>

#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
#define POOL_FIRST       16
#define POOL_SIZE        8
#define LEASE_S          (24 * 3600)

#define OPT_PAD         0
#define OPT_SUBNET_MASK 1
#define OPT_ROUTER      3
#define OPT_DNS         6
#define OPT_REQUESTED   50
#define OPT_LEASE_TIME  51
#define OPT_MSG_TYPE    53
#define OPT_SERVER_ID   54
#define OPT_END         255

#define MSG_DISCOVER 1
#define MSG_OFFER    2
#define MSG_REQUEST  3
#define MSG_ACK      5
#define MSG_NAK      6

typedef struct {
    uint8_t op, htype, hlen, hops;
    uint8_t xid[4];
    uint8_t secs[2], flags[2];
    uint8_t ciaddr[4], yiaddr[4], siaddr[4], giaddr[4];
    uint8_t chaddr[16];
    uint8_t sname[64];
    uint8_t file[128];
    uint8_t magic[4];
    uint8_t options[312];
} dhcp_msg_t;

static struct udp_pcb* pcb;
static struct netif* ap_nif;
static uint8_t lease_mac[POOL_SIZE][6];
static bool lease_used[POOL_SIZE];

static const uint8_t magic[4] = {99, 130, 83, 99};

// Find option `code` in the options area; returns pointer to its value and sets *len
static const uint8_t* opt_find(const uint8_t* o, int n, uint8_t code, uint8_t* len) {
    int i = 0;
    while (i < n) {
        uint8_t c = o[i];
        if (c == OPT_END) break;
        if (c == OPT_PAD) {
            i++;
            continue;
        }
        if (i + 1 >= n) break;
        uint8_t l = o[i + 1];
        if (i + 2 + l > n) break;
        if (c == code) {
            *len = l;
            return &o[i + 2];
        }
        i += 2 + l;
    }
    return NULL;
}

static int lease_for(const uint8_t* mac) {
    for (int i = 0; i < POOL_SIZE; i++)
        if (lease_used[i] && memcmp(lease_mac[i], mac, 6) == 0) return i;
    for (int i = 0; i < POOL_SIZE; i++)
        if (!lease_used[i]) {
            lease_used[i] = true;
            memcpy(lease_mac[i], mac, 6);
            return i;
        }
    return -1;   // pool full
}

static uint8_t* opt_put(uint8_t* p, uint8_t code, uint8_t len, const void* v) {
    *p++ = code;
    *p++ = len;
    memcpy(p, v, len);
    return p + len;
}

static void reply(const dhcp_msg_t* req, uint8_t type, int idx) {
    struct pbuf* p = pbuf_alloc(PBUF_TRANSPORT, sizeof(dhcp_msg_t), PBUF_RAM);
    if (!p) return;
    dhcp_msg_t* m = (dhcp_msg_t*)p->payload;
    memset(m, 0, sizeof(*m));
    m->op = 2;   // BOOTREPLY
    m->htype = req->htype;
    m->hlen = req->hlen;
    memcpy(m->xid, req->xid, 4);
    memcpy(m->flags, req->flags, 2);
    memcpy(m->chaddr, req->chaddr, 16);
    memcpy(m->magic, magic, 4);

    const ip4_addr_t* me = netif_ip4_addr(ap_nif);
    uint8_t server[4];
    memcpy(server, &me->addr, 4);   // network byte order already
    if (idx >= 0) {
        memcpy(m->yiaddr, server, 3);
        m->yiaddr[3] = (uint8_t)(POOL_FIRST + idx);
    }
    memcpy(m->siaddr, server, 4);

    uint8_t* o = m->options;
    uint8_t mask[4] = {255, 255, 255, 0};
    uint32_t lease = PP_HTONL(LEASE_S);
    o = opt_put(o, OPT_MSG_TYPE, 1, &type);
    o = opt_put(o, OPT_SERVER_ID, 4, server);
    if (type != MSG_NAK) {
        o = opt_put(o, OPT_SUBNET_MASK, 4, mask);
        o = opt_put(o, OPT_ROUTER, 4, server);
        o = opt_put(o, OPT_DNS, 4, server);
        o = opt_put(o, OPT_LEASE_TIME, 4, &lease);
    }
    *o++ = OPT_END;
    pbuf_realloc(p, (u16_t)(o - (uint8_t*)m));

    ip_addr_t bcast;
    IP_ADDR4(&bcast, 255, 255, 255, 255);
    udp_sendto_if(pcb, p, &bcast, DHCP_CLIENT_PORT, ap_nif);
    pbuf_free(p);
}

static void on_packet(void* arg, struct udp_pcb* upcb, struct pbuf* p, const ip_addr_t* addr, u16_t port) {
    (void)arg;
    (void)upcb;
    (void)addr;
    (void)port;
    dhcp_msg_t req;
    memset(&req, 0, sizeof(req));
    u16_t n = pbuf_copy_partial(p, &req, sizeof(req), 0);
    pbuf_free(p);
    if (n < 240 || req.op != 1 || memcmp(req.magic, magic, 4) != 0) return;   // not a BOOTREQUEST
    int optlen = n - 240;
    uint8_t l = 0;
    const uint8_t* t = opt_find(req.options, optlen, OPT_MSG_TYPE, &l);
    if (!t || l != 1) return;
    int idx = lease_for(req.chaddr);
    if (*t == MSG_DISCOVER) {
        if (idx >= 0) reply(&req, MSG_OFFER, idx);
    } else if (*t == MSG_REQUEST) {
        const uint8_t* want = opt_find(req.options, optlen, OPT_REQUESTED, &l);
        const uint8_t* me = (const uint8_t*)&netif_ip4_addr(ap_nif)->addr;
        bool ok = idx >= 0;
        if (ok && want && l == 4)   // must be the address we gave this client
            ok = memcmp(want, me, 3) == 0 && want[3] == POOL_FIRST + idx;
        reply(&req, ok ? MSG_ACK : MSG_NAK, ok ? idx : -1);
    }
}

void dhcpd_start(struct netif* nif) {
    dhcpd_stop();
    ap_nif = nif;
    memset(lease_used, 0, sizeof(lease_used));
    pcb = udp_new();
    if (!pcb) return;
    ip_set_option(pcb, SOF_BROADCAST);
    udp_bind_netif(pcb, nif);
    if (udp_bind(pcb, IP_ANY_TYPE, DHCP_SERVER_PORT) != ERR_OK) {
        udp_remove(pcb);
        pcb = NULL;
        return;
    }
    udp_recv(pcb, on_packet, NULL);
}

void dhcpd_stop(void) {
    if (pcb) udp_remove(pcb);
    pcb = NULL;
}
