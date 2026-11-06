#include "dnsd.h"

#include <string.h>

#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"

#define DNS_PORT 53
#define MAX_MSG  512

static struct udp_pcb* pcb;
static struct netif* ap_nif;

static void on_query(void* arg, struct udp_pcb* upcb, struct pbuf* p, const ip_addr_t* addr, u16_t port) {
    (void)arg;
    uint8_t q[MAX_MSG];
    u16_t n = pbuf_copy_partial(p, q, sizeof(q), 0);
    pbuf_free(p);
    if (n < 12 || (q[2] & 0x80)) return;   // too short, or a response
    if (q[4] != 0 || q[5] != 1) return;     // exactly one question

    // walk the question name to find its end (no compression in questions)
    int i = 12;
    while (i < n && q[i]) {
        if (q[i] & 0xC0) return;
        i += q[i] + 1;
    }
    if (i + 5 > n) return;
    int qend = i + 5;   // zero byte + type(2) + class(2)
    uint16_t qtype = (uint16_t)((q[i + 1] << 8) | q[i + 2]);

    uint8_t r[MAX_MSG + 16];
    memcpy(r, q, (size_t)qend);
    r[2] = 0x84 | (q[2] & 0x01);   // response, authoritative, copy RD
    r[3] = 0x00;                   // no error
    r[6] = 0;
    r[7] = (qtype == 1) ? 1 : 0;   // answer only A queries (others: empty, so phones fall back to IPv4)
    r[8] = r[9] = r[10] = r[11] = 0;
    int k = qend;
    if (qtype == 1) {
        const uint8_t* ip = (const uint8_t*)&netif_ip4_addr(ap_nif)->addr;
        const uint8_t ans[] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4};   // name ptr, A, IN, TTL 30 s, len 4
        memcpy(r + k, ans, sizeof(ans));
        k += sizeof(ans);
        memcpy(r + k, ip, 4);
        k += 4;
    }
    struct pbuf* out = pbuf_alloc(PBUF_TRANSPORT, (u16_t)k, PBUF_RAM);
    if (!out) return;
    memcpy(out->payload, r, (size_t)k);
    udp_sendto_if(upcb, out, addr, port, ap_nif);
    pbuf_free(out);
}

void dnsd_start(struct netif* nif) {
    dnsd_stop();
    ap_nif = nif;
    pcb = udp_new();
    if (!pcb) return;
    udp_bind_netif(pcb, nif);
    if (udp_bind(pcb, IP_ANY_TYPE, DNS_PORT) != ERR_OK) {
        udp_remove(pcb);
        pcb = NULL;
        return;
    }
    udp_recv(pcb, on_query, NULL);
}

void dnsd_stop(void) {
    if (pcb) udp_remove(pcb);
    pcb = NULL;
}
