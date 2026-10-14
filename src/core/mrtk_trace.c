/*------------------------------------------------------------------------------
 * mrtk_trace.c : debug trace and logging functions
 *
 * Copyright (C) 2026 H.SHIONO (MRTKLIB Project)
 * Copyright (C) 2023-2025 Cabinet Office, Japan
 * Copyright (C) 2024-2025 Lighthouse Technology & Consulting Co. Ltd.
 * Copyright (C) 2023-2025 Japan Aerospace Exploration Agency
 * Copyright (C) 2023-2025 TOSHIBA ELECTRONIC TECHNOLOGIES CORPORATION
 * Copyright (C) 2015- Mitsubishi Electric Corp.
 * Copyright (C) 2014 Geospatial Information Authority of Japan
 * Copyright (C) 2014 T.SUZUKI
 * Copyright (C) 2007-2023 T.TAKASU
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *----------------------------------------------------------------------------*/
#include "mrtklib/mrtk_trace.h"

#include "mrtklib/mrtk_nav.h"
#include "mrtklib/mrtk_obs.h"
#include "mrtklib/mrtk_sys.h"
#include "mrtklib/mrtk_time.h"

/**
 * @brief Resolve context: use explicit ctx if non-NULL, else fall back to global.
 */
#define CTX(c) ((c) ? (c) : g_mrtk_ctx)

#ifdef TRACE

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct mrtk_trace_sink {
    mrtk_trace_sink_cb_t fn;
    void* arg;
    int max_level;
    atomic_int level;
};

extern int tracesink(mrtk_ctx_t* ctx, mrtk_trace_sink_cb_t fn, void* arg, int max_level) {
    mrtk_ctx_t* c = CTX(ctx);
    mrtk_trace_sink_t* sink;

    if (!c || !fn || max_level < 1 || c->trace_sink) {
        return 0;
    }
    if (!(sink = (mrtk_trace_sink_t*)malloc(sizeof(mrtk_trace_sink_t)))) {
        return 0;
    }
    sink->fn = fn;
    sink->arg = arg;
    sink->max_level = max_level;
    atomic_init(&sink->level, 0);
    c->trace_sink = sink;
    return 1;
}
extern int tracesinklevel(mrtk_ctx_t* ctx, int level) {
    mrtk_ctx_t* c = CTX(ctx);

    if (!c || !c->trace_sink || level < 0 || level > c->trace_sink->max_level) {
        return 0;
    }
    atomic_store_explicit(&c->trace_sink->level, level, memory_order_relaxed);
    return 1;
}
/* Keep the message buffer off the file-only trace path's stack. */
static void trace_sink_message(mrtk_trace_sink_t* sink, int level, const char* format, va_list ap) {
    char msg[MRTK_TRACE_SINK_MSGLEN];

    vsnprintf(msg, sizeof(msg), format, ap);
    msg[sizeof(msg) - 1] = '\0';
    sink->fn(sink->arg, level, msg);
}

extern void traceopen(mrtk_ctx_t* ctx, const char* file) {
    mrtk_ctx_t* c = CTX(ctx);
    if (!c) {
        return;
    }

    /* Close existing trace file if any */
    if (c->trace_fp && c->trace_fp != stdout && c->trace_fp != stderr) {
        fclose(c->trace_fp);
        c->trace_fp = NULL;
    }

    if (file && *file) {
        /* expand time/station keywords (%Y %m %d %h %M ...) like stream paths do (#83);
           reppath() does an unbounded strcpy into rpath, so skip expansion for an
           over-long path rather than overflow the buffer */
        char rpath[MAXSTRPATH];
        if (strlen(file) < sizeof(rpath)) {
            reppath(file, rpath, utc2gpst(timeget()), "", "");
            c->trace_fp = fopen(rpath, "w");
        } else {
            c->trace_fp = fopen(file, "w");
        }
    }
    c->tick_trace = tickget();
}
extern void traceclose(mrtk_ctx_t* ctx) {
    mrtk_ctx_t* c = CTX(ctx);
    if (!c) {
        return;
    }

    if (c->trace_fp && c->trace_fp != stdout && c->trace_fp != stderr) {
        fclose(c->trace_fp);
    }
    c->trace_fp = NULL;
}
extern void tracelevel(mrtk_ctx_t* ctx, int level) {
    mrtk_ctx_t* c = CTX(ctx);
    if (!c) {
        return;
    }
    c->trace_level = level;
}
extern void trace(mrtk_ctx_t* ctx, int level, const char* format, ...) {
    mrtk_ctx_t* c = CTX(ctx);
    int file, sink;
    va_list ap;

    if (!c || level < 1) {
        return;
    }
    file = c->trace_fp && level <= c->trace_level;
    sink = c->trace_sink && level <= atomic_load_explicit(&c->trace_sink->level, memory_order_relaxed);
    if (!file && !sink) {
        return;
    }
    if (file) {
        va_start(ap, format);
        vfprintf(c->trace_fp, format, ap);
        va_end(ap);
        fflush(c->trace_fp);
    }
    if (sink) {
        va_start(ap, format);
        trace_sink_message(c->trace_sink, level, format, ap);
        va_end(ap);
    }
}
extern void tracet(mrtk_ctx_t* ctx, int level, const char* format, ...) {
    mrtk_ctx_t* c = CTX(ctx);
    int file, sink;
    va_list ap;

    if (!c || level < 1) {
        return;
    }
    file = c->trace_fp && level <= c->trace_level;
    sink = c->trace_sink && level <= atomic_load_explicit(&c->trace_sink->level, memory_order_relaxed);
    if (!file && !sink) {
        return;
    }
    if (file) {
        fprintf(c->trace_fp, "%d %9.3f: ", level, (tickget() - c->tick_trace) / 1000.0);
        va_start(ap, format);
        vfprintf(c->trace_fp, format, ap);
        va_end(ap);
        fflush(c->trace_fp);
    }
    if (sink) {
        va_start(ap, format);
        trace_sink_message(c->trace_sink, level, format, ap);
        va_end(ap);
    }
}
extern void tracemat(mrtk_ctx_t* ctx, int level, const double* A, int n, int m, int p, int q) {
    mrtk_ctx_t* c = CTX(ctx);
    int i, j;

    if (!c || !c->trace_fp || level < 1 || level > c->trace_level) {
        return;
    }

    for (i = 0; i < n; i++) {
        for (j = 0; j < m; j++) {
            fprintf(c->trace_fp, " %*.*f", p, q, A[i + j * n]);
        }
        fprintf(c->trace_fp, "\n");
    }
    fflush(c->trace_fp);
}
extern void traceobs(mrtk_ctx_t* ctx, int level, const obsd_t* obs, int n) {
    mrtk_ctx_t* c = CTX(ctx);
    char str[64], id[16];
    int i;

    if (!c || !c->trace_fp || level < 1 || level > c->trace_level) {
        return;
    }

    for (i = 0; i < n; i++) {
        time2str(obs[i].time, str, 3);
        satno2id(obs[i].sat, id);
        fprintf(c->trace_fp,
                " (%2d) %s %-3s rcv%d %13.3f %13.3f %13.3f %13.3f"
                " %d %d %d %d %3.1f %3.1f\n",
                i + 1, str, id, obs[i].rcv, obs[i].L[0], obs[i].L[1], obs[i].P[0], obs[i].P[1], obs[i].LLI[0],
                obs[i].LLI[1], obs[i].code[0], obs[i].code[1], obs[i].SNR[0] * SNR_UNIT, obs[i].SNR[1] * SNR_UNIT);
    }
    fflush(c->trace_fp);
}
extern void tracenav(mrtk_ctx_t* ctx, int level, const nav_t* nav) {
    mrtk_ctx_t* c = CTX(ctx);
    char s1[64], s2[64], id[16];
    int i;

    if (!c || !c->trace_fp || level < 1 || level > c->trace_level) {
        return;
    }

    for (i = 0; i < nav->n; i++) {
        time2str(nav->eph[i].toe, s1, 0);
        time2str(nav->eph[i].ttr, s2, 0);
        satno2id(nav->eph[i].sat, id);
        fprintf(c->trace_fp, "(%3d) %-3s : %s %s %3d %3d %02x\n", i + 1, id, s1, s2, nav->eph[i].iode, nav->eph[i].iodc,
                nav->eph[i].svh);
    }
    fprintf(c->trace_fp, "(ion) %9.4e %9.4e %9.4e %9.4e\n", nav->ion_gps[0], nav->ion_gps[1], nav->ion_gps[2],
            nav->ion_gps[3]);
    fprintf(c->trace_fp, "(ion) %9.4e %9.4e %9.4e %9.4e\n", nav->ion_gps[4], nav->ion_gps[5], nav->ion_gps[6],
            nav->ion_gps[7]);
    fprintf(c->trace_fp, "(ion) %9.4e %9.4e %9.4e %9.4e\n", nav->ion_gal[0], nav->ion_gal[1], nav->ion_gal[2],
            nav->ion_gal[3]);
    fflush(c->trace_fp);
}
extern void tracegnav(mrtk_ctx_t* ctx, int level, const nav_t* nav) {
    mrtk_ctx_t* c = CTX(ctx);
    char s1[64], s2[64], id[16];
    int i;

    if (!c || !c->trace_fp || level < 1 || level > c->trace_level) {
        return;
    }

    for (i = 0; i < nav->ng; i++) {
        time2str(nav->geph[i].toe, s1, 0);
        time2str(nav->geph[i].tof, s2, 0);
        satno2id(nav->geph[i].sat, id);
        fprintf(c->trace_fp, "(%3d) %-3s : %s %s %2d %2d %8.3f\n", i + 1, id, s1, s2, nav->geph[i].frq,
                nav->geph[i].svh, nav->geph[i].taun * 1E6);
    }
    fflush(c->trace_fp);
}
extern void tracehnav(mrtk_ctx_t* ctx, int level, const nav_t* nav) {
    mrtk_ctx_t* c = CTX(ctx);
    char s1[64], s2[64], id[16];
    int i;

    if (!c || !c->trace_fp || level < 1 || level > c->trace_level) {
        return;
    }

    for (i = 0; i < nav->ns; i++) {
        time2str(nav->seph[i].t0, s1, 0);
        time2str(nav->seph[i].tof, s2, 0);
        satno2id(nav->seph[i].sat, id);
        fprintf(c->trace_fp, "(%3d) %-3s : %s %s %2d %2d\n", i + 1, id, s1, s2, nav->seph[i].svh, nav->seph[i].sva);
    }
    fflush(c->trace_fp);
}
extern void tracepeph(mrtk_ctx_t* ctx, int level, const nav_t* nav) {
    mrtk_ctx_t* c = CTX(ctx);
    char s[64], id[16];
    int i, j;

    if (!c || !c->trace_fp || level < 1 || level > c->trace_level) {
        return;
    }

    for (i = 0; i < nav->ne; i++) {
        time2str(nav->peph[i].time, s, 0);
        for (j = 0; j < MAXSAT; j++) {
            satno2id(j + 1, id);
            fprintf(c->trace_fp,
                    "%-3s %d %-3s %13.3f %13.3f %13.3f %13.3f"
                    " %6.3f %6.3f %6.3f %6.3f\n",
                    s, nav->peph[i].index, id, nav->peph[i].pos[j][0], nav->peph[i].pos[j][1], nav->peph[i].pos[j][2],
                    nav->peph[i].pos[j][3] * 1E9, nav->peph[i].std[j][0], nav->peph[i].std[j][1],
                    nav->peph[i].std[j][2], nav->peph[i].std[j][3] * 1E9);
        }
    }
    fflush(c->trace_fp);
}
extern void tracepclk(mrtk_ctx_t* ctx, int level, const nav_t* nav) {
    mrtk_ctx_t* c = CTX(ctx);
    char s[64], id[16];
    int i, j;

    if (!c || !c->trace_fp || level < 1 || level > c->trace_level) {
        return;
    }

    for (i = 0; i < nav->nc; i++) {
        time2str(nav->pclk[i].time, s, 0);
        for (j = 0; j < MAXSAT; j++) {
            satno2id(j + 1, id);
            fprintf(c->trace_fp, "%-3s %d %-3s %13.3f %6.3f\n", s, nav->pclk[i].index, id, nav->pclk[i].clk[j][0] * 1E9,
                    nav->pclk[i].std[j][0] * 1E9);
        }
    }
    fflush(c->trace_fp);
}
extern void traceb(mrtk_ctx_t* ctx, int level, const uint8_t* p, int n) {
    mrtk_ctx_t* c = CTX(ctx);
    int i;

    if (!c || !c->trace_fp || level < 1 || level > c->trace_level) {
        return;
    }

    for (i = 0; i < n; i++) {
        fprintf(c->trace_fp, "%02X%s", *p++, i % 8 == 7 ? " " : "");
    }
    fprintf(c->trace_fp, "\n");
    fflush(c->trace_fp);
}
#else
extern int tracesink(mrtk_ctx_t* ctx, mrtk_trace_sink_cb_t fn, void* arg, int max_level) {
    (void)ctx;
    (void)fn;
    (void)arg;
    (void)max_level;
    return 0;
}
extern int tracesinklevel(mrtk_ctx_t* ctx, int level) {
    (void)ctx;
    (void)level;
    return 0;
}
extern void traceopen(mrtk_ctx_t* ctx, const char* file) {
    (void)ctx;
    (void)file;
}
extern void traceclose(mrtk_ctx_t* ctx) { (void)ctx; }
extern void tracelevel(mrtk_ctx_t* ctx, int level) {
    (void)ctx;
    (void)level;
}
extern void trace(mrtk_ctx_t* ctx, int level, const char* format, ...) {
    (void)ctx;
    (void)level;
    (void)format;
}
extern void tracet(mrtk_ctx_t* ctx, int level, const char* format, ...) {
    (void)ctx;
    (void)level;
    (void)format;
}
extern void tracemat(mrtk_ctx_t* ctx, int level, const double* A, int n, int m, int p, int q) {
    (void)ctx;
    (void)level;
    (void)A;
    (void)n;
    (void)m;
    (void)p;
    (void)q;
}
extern void traceobs(mrtk_ctx_t* ctx, int level, const obsd_t* obs, int n) {
    (void)ctx;
    (void)level;
    (void)obs;
    (void)n;
}
extern void tracenav(mrtk_ctx_t* ctx, int level, const nav_t* nav) {
    (void)ctx;
    (void)level;
    (void)nav;
}
extern void tracegnav(mrtk_ctx_t* ctx, int level, const nav_t* nav) {
    (void)ctx;
    (void)level;
    (void)nav;
}
extern void tracehnav(mrtk_ctx_t* ctx, int level, const nav_t* nav) {
    (void)ctx;
    (void)level;
    (void)nav;
}
extern void tracepeph(mrtk_ctx_t* ctx, int level, const nav_t* nav) {
    (void)ctx;
    (void)level;
    (void)nav;
}
extern void tracepclk(mrtk_ctx_t* ctx, int level, const nav_t* nav) {
    (void)ctx;
    (void)level;
    (void)nav;
}
extern void traceb(mrtk_ctx_t* ctx, int level, const uint8_t* p, int n) {
    (void)ctx;
    (void)level;
    (void)p;
    (void)n;
}

#endif /* TRACE */
