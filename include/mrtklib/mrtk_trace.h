/*------------------------------------------------------------------------------
 * mrtk_trace.h : debug trace and logging functions
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
/**
 * @file mrtk_trace.h
 * @brief MRTKLIB Trace Module — debug trace and logging functions.
 *
 * All trace functions accept an explicit mrtk_ctx_t pointer as their first
 * argument.  When ctx is NULL, the global g_mrtk_ctx is used as a fallback.
 *
 * @note Functions declared here are implemented in mrtk_trace.c.
 */
#ifndef MRTK_TRACE_H
#define MRTK_TRACE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "mrtklib/mrtk_context.h"
#include "mrtklib/mrtk_nav.h"
#include "mrtklib/mrtk_obs.h"

/*============================================================================
 * Debug Trace Functions
 *===========================================================================*/

/** Maximum sink message size in bytes, including the terminating NUL. */
#define MRTK_TRACE_SINK_MSGLEN 1024

/**
 * @brief Receive one formatted trace()/tracet() message and its call level.
 *
 * The message includes any newline from the call, but no tracet() time prefix.
 * It is truncated to MRTK_TRACE_SINK_MSGLEN - 1 bytes, always NUL-terminated,
 * and valid only during the callback. Dump functions remain file-only.
 * The callback runs in the tracing thread, possibly concurrently and while
 * library locks (server or stream locks, for example) are held. It must be
 * thread-safe, return quickly, and must not call trace functions or acquire
 * locks the library may hold while tracing. The callback and its argument
 * must stay valid until every thread that traces with the context has
 * stopped; setting the level to 0 does not wait for callbacks already running.
 *
 * @param[in] arg    User argument supplied to tracesink()
 * @param[in] level  Trace call level
 * @param[in] msg    Formatted message
 */
typedef void (*mrtk_trace_sink_cb_t)(void* arg, int level, const char* msg);

/**
 * @brief Register a trace sink once for the context's lifetime (no unregister).
 *
 * Register before starting other threads that trace with this context.
 * The initial sink level is 0 (off). The sink receives calls at levels
 * 1 through its current level, independently of trace file settings.
 * Levels 3 and above may contain secrets (passwords in commands, stream paths
 * and NTRIP requests); choose max_level accordingly (2 for low-level logs).
 * The context owns the sink storage, but never frees arg.
 *
 * @param[in] ctx        Runtime context (NULL = use global)
 * @param[in] fn         Sink callback (must not be NULL)
 * @param[in] arg        User callback argument
 * @param[in] max_level  Maximum permitted sink level (at least 1)
 * @return 1 on success; 0 for no context, invalid arguments, an existing sink,
 *         allocation failure, or a build without TRACE.
 */
int tracesink(mrtk_ctx_t* ctx, mrtk_trace_sink_cb_t fn, void* arg, int max_level);

/**
 * @brief Set the sink level from any thread, effective for the next trace call.
 * @param[in] ctx    Runtime context (NULL = use global)
 * @param[in] level  Sink level (0 = off, up to the registered max_level)
 * @return 1 on success; 0 with the level unchanged for no context/sink,
 *         an out-of-range level (not clamped), or a build without TRACE.
 */
int tracesinklevel(mrtk_ctx_t* ctx, int level);

/**
 * @brief Open trace output (initializes tick counter for tracet).
 * @param[in] ctx   Runtime context (NULL = use global)
 * @param[in] file  Trace file path (opened for writing)
 */
void traceopen(mrtk_ctx_t* ctx, const char* file);

/**
 * @brief Close trace output.
 * @param[in] ctx  Runtime context (NULL = use global)
 */
void traceclose(mrtk_ctx_t* ctx);

/**
 * @brief Set trace level.
 * @param[in] ctx    Runtime context (NULL = use global)
 * @param[in] level  Trace level (0:off, 1:error, 2:warn, 3:info, 4-5:debug)
 */
void tracelevel(mrtk_ctx_t* ctx, int level);

/**
 * @brief Output trace message.
 * @param[in] ctx     Runtime context (NULL = use global)
 * @param[in] level   Trace level
 * @param[in] format  printf-style format string
 * @param[in] ...     Format arguments
 */
void trace(mrtk_ctx_t* ctx, int level, const char* format, ...);

/**
 * @brief Output trace message with elapsed time.
 * @param[in] ctx     Runtime context (NULL = use global)
 * @param[in] level   Trace level
 * @param[in] format  printf-style format string
 * @param[in] ...     Format arguments
 */
void tracet(mrtk_ctx_t* ctx, int level, const char* format, ...);

/**
 * @brief Output trace matrix.
 * @param[in] ctx    Runtime context (NULL = use global)
 * @param[in] level  Trace level
 * @param[in] A      Matrix data (column-major)
 * @param[in] n      Number of rows
 * @param[in] m      Number of columns
 * @param[in] p      Total field width
 * @param[in] q      Decimal places
 */
void tracemat(mrtk_ctx_t* ctx, int level, const double* A, int n, int m, int p, int q);

/**
 * @brief Output trace observation data.
 * @param[in] ctx    Runtime context (NULL = use global)
 * @param[in] level  Trace level
 * @param[in] obs    Observation data array
 * @param[in] n      Number of observations
 */
void traceobs(mrtk_ctx_t* ctx, int level, const obsd_t* obs, int n);

/**
 * @brief Output trace navigation data (GPS ephemeris).
 * @param[in] ctx    Runtime context (NULL = use global)
 * @param[in] level  Trace level
 * @param[in] nav    Navigation data
 */
void tracenav(mrtk_ctx_t* ctx, int level, const nav_t* nav);

/**
 * @brief Output trace GLONASS navigation data.
 * @param[in] ctx    Runtime context (NULL = use global)
 * @param[in] level  Trace level
 * @param[in] nav    Navigation data
 */
void tracegnav(mrtk_ctx_t* ctx, int level, const nav_t* nav);

/**
 * @brief Output trace SBAS navigation data.
 * @param[in] ctx    Runtime context (NULL = use global)
 * @param[in] level  Trace level
 * @param[in] nav    Navigation data
 */
void tracehnav(mrtk_ctx_t* ctx, int level, const nav_t* nav);

/**
 * @brief Output trace precise ephemeris data.
 * @param[in] ctx    Runtime context (NULL = use global)
 * @param[in] level  Trace level
 * @param[in] nav    Navigation data
 */
void tracepeph(mrtk_ctx_t* ctx, int level, const nav_t* nav);

/**
 * @brief Output trace precise clock data.
 * @param[in] ctx    Runtime context (NULL = use global)
 * @param[in] level  Trace level
 * @param[in] nav    Navigation data
 */
void tracepclk(mrtk_ctx_t* ctx, int level, const nav_t* nav);

/**
 * @brief Output trace binary data.
 * @param[in] ctx    Runtime context (NULL = use global)
 * @param[in] level  Trace level
 * @param[in] p      Binary data
 * @param[in] n      Data length (bytes)
 */
void traceb(mrtk_ctx_t* ctx, int level, const uint8_t* p, int n);

#ifdef __cplusplus
}
#endif

#endif /* MRTK_TRACE_H */
