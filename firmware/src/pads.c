#include "pads.h"

#include <string.h>

pad_t pads[MAX_PADS];

void ring_clear(ring_t* r) { r->head = r->tail = 0; }

void ring_push(ring_t* r, const uint8_t* data, uint8_t len) {
    uint8_t next = (uint8_t)((r->head + 1) % RING_SIZE);
    if (next == r->tail) r->tail = (uint8_t)((r->tail + 1) % RING_SIZE);   // full: drop oldest
    if (len > sizeof(r->p[0].data)) len = sizeof(r->p[0].data);
    memcpy(r->p[r->head].data, data, len);
    r->p[r->head].len = len;
    r->head = next;
}

bool ring_pop(ring_t* r, pkt_t* out) {
    if (r->tail == r->head) return false;
    *out = r->p[r->tail];
    r->tail = (uint8_t)((r->tail + 1) % RING_SIZE);
    return true;
}
