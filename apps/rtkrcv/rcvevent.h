/*------------------------------------------------------------------------------
 * rcvevent.h : event buffer of the rtk receiver command layer
 *
 * Copyright (C) 2026 H.SHIONO (MRTKLIB Project)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *----------------------------------------------------------------------------*/
/**
 * @file rcvevent.h
 * @brief Fixed-capacity ring of sequence-numbered events (status, solution, log).
 *
 * One sequence covers all kinds; it starts at 1, increases in publish order and
 * is never reset. The ring allocates only in rcvevent_init; publishing and
 * reading copy fixed-size entries. It has no lock of its own: the command
 * layer calls every function except init/free under its publish lock.
 * This header stays C90-compatible because rtkrcv.c (-ansi) includes it.
 */
#ifndef RCVEVENT_H
#define RCVEVENT_H

#include <stddef.h>
#include <stdint.h>

#include "mrtklib/mrtk_sol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RCVEVENT_TEXT 512 /* log text bytes including the terminating NUL */

typedef enum { RCVEVENT_STATUS, RCVEVENT_SOLUTION, RCVEVENT_LOG } rcvevent_kind_t;

/* origin of a log message: the telnet error command shows errbuf ones only */
typedef enum { RCVEVENT_LOG_LAYER, RCVEVENT_LOG_ERRBUF } rcvevent_log_origin_t;

typedef struct {
    int state;       /* rcvcmd_state_t */
    int stop_reason; /* rcvcmd_stop_reason_t */
    int last_error;  /* rcvcmd_start_error_t */
} rcvevent_status_t;

typedef struct {
    sol_t sol;          /* the solution as the server produced it */
    double rb[3];       /* rtk.rb (ecef) when collected, as the telnet display uses it */
    int has_base;       /* the mode uses a base and rb is known (RPC `base`) */
    double leap;        /* GPST - UTC (s) for sol.time */
    int geoid_model;    /* solopt geoid model of the run */
    int has_separation; /* separation is valid */
    double separation;  /* geoid height above the ellipsoid (m) */
} rcvevent_solution_t;

typedef struct {
    int level;  /* 1, 2 */
    int origin; /* rcvevent_log_origin_t */
    char text[RCVEVENT_TEXT];
} rcvevent_log_t;

typedef struct {
    uint64_t seq; /* > 0 once published */
    int kind;     /* rcvevent_kind_t */
    union {
        rcvevent_status_t status;
        rcvevent_solution_t solution;
        rcvevent_log_t log;
    } data;
} rcvevent_t;

typedef struct {
    rcvevent_t* buf;
    size_t cap;
    uint64_t last; /* last published seq, 0 when none */
} rcvevent_ring_t;

typedef struct {
    int n;           /* events copied */
    uint64_t next;   /* seq of the last copied event, or after when none */
    int more;        /* more events are ready now */
    int lost;        /* events after `after` are no longer held; n is 0 */
    uint64_t oldest; /* with lost: the oldest seq held */
} rcvevent_read_t;

/**
 * @brief Allocate an empty ring.
 * @param[out] ring Ring to initialize.
 * @param[in]  cap  Number of entries (> 0).
 * @return 1 on success, 0 on allocation failure.
 */
int rcvevent_init(rcvevent_ring_t* ring, size_t cap);
/** @brief Free the ring storage. */
void rcvevent_free(rcvevent_ring_t* ring);
/**
 * @brief Copy an event into the ring with the next seq, dropping the oldest when full.
 * @param[in,out] ring Ring.
 * @param[in]     ev   Event; its seq member is ignored.
 * @return The assigned seq.
 */
uint64_t rcvevent_publish(rcvevent_ring_t* ring, const rcvevent_t* ev);
/** @brief Return the oldest seq held, 0 when nothing was published. */
uint64_t rcvevent_oldest(const rcvevent_ring_t* ring);
/**
 * @brief Return the entry of a seq, or NULL when it is not held.
 * The pointer is valid until the next publish.
 */
const rcvevent_t* rcvevent_get(const rcvevent_ring_t* ring, uint64_t seq);
/**
 * @brief Copy events with seq > after in ascending order (getEvents).
 * @param[in]  ring  Ring.
 * @param[in]  after Cursor: the last seq the reader has.
 * @param[out] out   At least limit entries.
 * @param[in]  limit Maximum number of events to copy (> 0).
 * @param[out] res   Copied count, next cursor, more, lost and oldest held seq.
 * @return 0 on success, -1 when after is greater than the last published seq.
 */
int rcvevent_read(const rcvevent_ring_t* ring, uint64_t after, rcvevent_t* out, int limit, rcvevent_read_t* res);
/**
 * @brief Copy the newest events of one kind with seq > after, oldest first.
 * Events no longer held are skipped without notice (telnet `solution` shows
 * the newest ones).
 * @param[in]  ring  Ring.
 * @param[in]  after Cursor (not greater than the last published seq).
 * @param[in]  kind  rcvevent_kind_t to copy.
 * @param[out] out   At least limit entries.
 * @param[in]  limit Maximum number of events to copy (> 0).
 * @param[out] next  Next cursor: the last published seq.
 * @return Number of events copied.
 */
int rcvevent_read_newest(const rcvevent_ring_t* ring, uint64_t after, int kind, rcvevent_t* out, int limit,
                         uint64_t* next);

#ifdef __cplusplus
}
#endif

#endif /* RCVEVENT_H */
