/*------------------------------------------------------------------------------
 * rcvevent.c : event buffer of the rtk receiver command layer
 *
 * Copyright (C) 2026 H.SHIONO (MRTKLIB Project)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *----------------------------------------------------------------------------*/
#include "rcvevent.h"

#include <stdlib.h>

/* seq s lives at (s - 1) % cap: the held range is always contiguous */
static rcvevent_t* slot(const rcvevent_ring_t* ring, uint64_t seq) { return ring->buf + (seq - 1) % ring->cap; }

int rcvevent_init(rcvevent_ring_t* ring, size_t cap) {
    ring->buf = (rcvevent_t*)calloc(cap, sizeof(rcvevent_t));
    ring->cap = cap;
    ring->last = 0;
    return ring->buf != NULL;
}

void rcvevent_free(rcvevent_ring_t* ring) {
    free(ring->buf);
    ring->buf = NULL;
    ring->cap = 0;
}

uint64_t rcvevent_publish(rcvevent_ring_t* ring, const rcvevent_t* ev) {
    rcvevent_t* entry = slot(ring, ring->last + 1);

    *entry = *ev;
    entry->seq = ++ring->last;
    return ring->last;
}

uint64_t rcvevent_oldest(const rcvevent_ring_t* ring) {
    if (ring->last == 0) {
        return 0;
    }
    return ring->last > ring->cap ? ring->last - ring->cap + 1 : 1;
}

const rcvevent_t* rcvevent_get(const rcvevent_ring_t* ring, uint64_t seq) {
    if (seq == 0 || seq > ring->last || seq < rcvevent_oldest(ring)) {
        return NULL;
    }
    return slot(ring, seq);
}

int rcvevent_read(const rcvevent_ring_t* ring, uint64_t after, rcvevent_t* out, int limit, rcvevent_read_t* res) {
    uint64_t seq;

    res->n = 0;
    res->next = after;
    res->more = 0;
    res->lost = 0;
    res->oldest = 0;
    if (after > ring->last) {
        return -1;
    }
    if (after < ring->last && after + 1 < rcvevent_oldest(ring)) {
        res->lost = 1;
        res->oldest = rcvevent_oldest(ring);
        return 0;
    }
    for (seq = after + 1; seq <= ring->last && res->n < limit; seq++) {
        out[res->n++] = *slot(ring, seq);
    }
    res->next = after + (uint64_t)res->n;
    res->more = res->next < ring->last;
    return 0;
}

int rcvevent_read_newest(const rcvevent_ring_t* ring, uint64_t after, int kind, rcvevent_t* out, int limit,
                         uint64_t* next) {
    uint64_t low = rcvevent_oldest(ring), first = ring->last + 1, seq;
    int n = 0;

    *next = ring->last;
    if (low < after + 1) {
        low = after + 1;
    }
    /* low >= 1, so seq-- below cannot wrap before the loop ends */
    for (seq = ring->last; seq >= low && n < limit; seq--) {
        if (slot(ring, seq)->kind == kind) {
            first = seq;
            n++;
        }
    }
    n = 0;
    for (seq = first; seq <= ring->last; seq++) {
        if (slot(ring, seq)->kind == kind) {
            out[n++] = *slot(ring, seq);
        }
    }
    return n;
}
