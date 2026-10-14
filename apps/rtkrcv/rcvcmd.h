/*------------------------------------------------------------------------------
 * rcvcmd.h : rtk receiver command layer
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
 *-----------------------------------------------------------------------------*/
/**
 * @file rcvcmd.h
 * @brief Internal RTK receiver command layer shared by adapters.
 *
 * Owns server/monitor resources, startup/finish, start/stop, configuration and
 * locked copies of server data. Adapters own parsing, formatting, terminal
 * interaction, sockets, signals, and process runtime/trace setup and teardown.
 *
 * rcvcmd_t is opaque. Only one instance may exist while core sysopts remains
 * process-global; callers use a handle so that restriction can later be lifted
 * inside the layer. rcvcmd_io_t supplies synchronous notices and overwrite
 * confirmation, preserving output and question order without terminal coupling.
 *
 * Each display has one reader filling caller-owned storage under the same lock
 * as before the split. Solution/error reads consume the shared buffers. The
 * adapter prints after the reader unlocks. Readers allocate no large temporary
 * snapshots; callers keep one copy in their existing display stack frame.
 * Pointer/lock members copied inside rtk_t or stream_t are not live handles for
 * callers to dereference or use; displays consume only the copied values.
 *
 * Threading is unchanged from before the split. There are no lifecycle states,
 * reader counts, configuration mutex, events, claims, or new serialization.
 * Start/stop can still race with display/configuration operations as before.
 * Stream copies still use the server lock until stage 4. The legacy unlocked
 * cyclic-solution discard is retained until solution-event work (D-17).
 */
#ifndef RCVCMD_H
#define RCVCMD_H

#include "mrtklib/mrtk_options.h"
#include "mrtklib/mrtk_rtksvr.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rcvcmd rcvcmd_t;

typedef struct {
    opt_t* opt;
    int system;
    int index;
} rcvcmd_option_t;

typedef enum {
    RCVCMD_NOTICE_CORRECTION_ERROR,
    RCVCMD_NOTICE_NO_COMMAND_FILE,
    RCVCMD_NOTICE_NO_ANTENNA,
    RCVCMD_NOTICE_ANTENNA_FILE_ERROR,
    RCVCMD_NOTICE_GEOID_FILE_ERROR,
    RCVCMD_NOTICE_COMMAND_EXEC_ERROR,
    RCVCMD_NOTICE_SERVER_START_ERROR,
    RCVCMD_NOTICE_SERVER_STOPPED
} rcvcmd_notice_code_t;

/* Fields are borrowed for the synchronous callback only. text contains the
 * antenna name, command, or core error detail, never a formatted console line.
 * Raw command/error fields may contain credentials; a later RPC adapter must
 * construct its errors from the code/status and suitably masked fields. */
typedef struct {
    rcvcmd_notice_code_t code;
    const char* path;
    const char* text;
    int status;
} rcvcmd_notice_t;

typedef struct {
    void* arg;
    void (*notice)(void* arg, const rcvcmd_notice_t* notice);
    int (*confirm_overwrite)(void* arg, const char* path); /* 1: overwrite / no file, 0: abort */
} rcvcmd_io_t;

/** @brief Start the server using synchronous adapter callbacks.
 * @param[in,out] cmd The sole command-layer instance.
 * @param[in] io Adapter output and overwrite callbacks; both must be supplied.
 * @return 1 on success, 0 on failure or overwrite refusal.
 */
int rcvcmd_start(rcvcmd_t* cmd, const rcvcmd_io_t* io);
/** @brief Stop a running server; a stopped server emits no notice.
 * @param[in,out] cmd The sole command-layer instance.
 * @param[in] io Adapter output and overwrite callbacks; both must be supplied.
 */
void rcvcmd_stop(rcvcmd_t* cmd, const rcvcmd_io_t* io);

/** @brief Allocate the sole server instance; NULL on allocation failure or an existing instance. */
rcvcmd_t* rcvcmd_create(void);
/** @brief Free the server after the adapter has torn down its runtime context. */
void rcvcmd_destroy(rcvcmd_t* cmd);
/** @brief Initialize server/monitor, load startup options, navigation and status output.
 * @param[in,out] file Options path buffer (1024 bytes); filled with the default if empty.
 * @param[in] password Parsed -w value, applied before loading console-passwd from the file.
 */
void rcvcmd_initialize(rcvcmd_t* cmd, char* file, const char* password, const char* staname, int moniport, int outstat,
                       gtime_t rst);
/** @brief Close monitor and solution status before runtime context teardown. */
void rcvcmd_close(rcvcmd_t* cmd);
/** @brief Save navigation on normal shutdown, before trace/context teardown. */
void rcvcmd_save_navigation(rcvcmd_t* cmd);

/** @brief Read the configured console password. */
const char* rcvcmd_password(rcvcmd_t* cmd);
/** @brief Read the configured console time format. */
int rcvcmd_time_type(rcvcmd_t* cmd);
/** @brief Read the configured console solution format. */
int rcvcmd_solution_type(rcvcmd_t* cmd);
/** @brief Read the configured solution height mode at the original formatting points. */
int rcvcmd_solution_height(rcvcmd_t* cmd);
/** @brief Read the configured console solution flags. */
int rcvcmd_solution_flags(rcvcmd_t* cmd);
/** @brief Read the requested monitor port. */
int rcvcmd_monitor_port(rcvcmd_t* cmd);

/** @brief Get the ordered option table (system=0: receiver, system=1: core system). */
const opt_t* rcvcmd_options(rcvcmd_t* cmd, int system);
/** @brief Read the modified flag for an entry in the selected option table. */
int rcvcmd_option_modified(rcvcmd_t* cmd, int system, int index);
/** @brief Find a receiver option first, then a system option; return 1 if found. */
int rcvcmd_find_option(rcvcmd_t* cmd, const char* name, rcvcmd_option_t* option);
/** @brief Apply a value and refresh processing/file options; return 0 for an invalid value. */
int rcvcmd_set_option(rcvcmd_t* cmd, const rcvcmd_option_t* option, const char* value);
/** @brief Mark a non-console option modified; return 1 if restart text is required.
 * Called after the adapter prints the changed-option text to retain the old ordering. */
int rcvcmd_mark_option_modified(rcvcmd_t* cmd, const rcvcmd_option_t* option);
/** @brief Load system then receiver options, preserving partial changes on failure. */
int rcvcmd_load(rcvcmd_t* cmd, const char* file);
/** @brief Write receiver then system options with the original comment; return 1 on success. */
int rcvcmd_save(rcvcmd_t* cmd, const char* file);

typedef struct {
    rtk_t rtk;
    uint32_t nmsg2[3][100], nmsg3[3][400];
    int thread, cycle, state, rtkstat, nsat0, nsat1, prcout, nave;
    int cputime, nb[3], nmsg[3][10];
    double runtime;
    double x3[3], P_diag[3], xa3[3], Pa_diag[3];
    int has_x, has_P, has_xa, has_Pa;
} rcvcmd_status_t;

typedef struct {
    rtk_t rtk;
} rcvcmd_satellite_t;

typedef struct {
    obsd_t obs[MAXOBS * 2];
    int n;
} rcvcmd_observ_t;

typedef struct {
    eph_t eph[MAXSAT * 2];
    geph_t geph[MAXPRNGLO];
    double ion[8], utc[8];
    gtime_t time;
} rcvcmd_navidata_t;

typedef struct {
    ssr_t ssr[MAXSAT];
    gtime_t time;
} rcvcmd_ssr_t;

typedef struct {
    stream_t stream[9];
    int format[9];
} rcvcmd_stream_t;

typedef struct {
    char errbuf[MAXERRMSG + 1];
    int n;
} rcvcmd_error_t;

typedef struct {
    sol_t solbuf[MAXSOLBUF];
    double rb[3];
    int nsol;
} rcvcmd_solution_t;

/** @brief Copy status fields and the existing deep copies of vector values.
 * @param[in] cmd The sole command-layer instance.
 * @param[out] data Caller-owned storage, filled directly without another snapshot copy.
 */
void rcvcmd_read_status(rcvcmd_t* cmd, rcvcmd_status_t* data);
/** @brief Copy the satellite display's RTK values.
 * @param[in] cmd The sole command-layer instance.
 * @param[out] data Caller-owned storage, filled directly without another snapshot copy.
 */
void rcvcmd_read_satellite(rcvcmd_t* cmd, rcvcmd_satellite_t* data);
/** @brief Copy rover then base observations matching sys, with the existing cap.
 * @param[in] cmd The sole command-layer instance.
 * @param[in] sys Satellite-system mask, with the original observation filtering.
 * @param[out] data Caller-owned storage, filled directly without another snapshot copy.
 */
void rcvcmd_read_observ(rcvcmd_t* cmd, int sys, rcvcmd_observ_t* data);
/** @brief Copy navigation time, ephemerides, GPS ionosphere and UTC parameters.
 * @param[in] cmd The sole command-layer instance.
 * @param[out] data Caller-owned storage, filled directly without another snapshot copy.
 */
void rcvcmd_read_navidata(rcvcmd_t* cmd, rcvcmd_navidata_t* data);
/** @brief Copy solution time and channel-zero SSR values.
 * @param[in] cmd The sole command-layer instance.
 * @param[out] data Caller-owned storage, filled directly without another snapshot copy.
 */
void rcvcmd_read_ssr(rcvcmd_t* cmd, rcvcmd_ssr_t* data);
/** @brief Copy streams, monitor and formats under the legacy server lock.
 * @param[in] cmd The sole command-layer instance.
 * @param[out] data Caller-owned storage, filled directly without another snapshot copy.
 */
void rcvcmd_read_stream(rcvcmd_t* cmd, rcvcmd_stream_t* data);
/** @brief Copy and clear pending errors; errbuf is terminated, n retains the byte count.
 * @param[in] cmd The sole command-layer instance.
 * @param[out] data Caller-owned storage, filled directly without another snapshot copy.
 */
void rcvcmd_read_error(rcvcmd_t* cmd, rcvcmd_error_t* data);
/** @brief Copy and clear pending solutions; only rows below nsol and their rb are valid.
 * @param[in] cmd The sole command-layer instance.
 * @param[out] data Caller-owned storage, filled directly without another snapshot copy.
 */
void rcvcmd_read_solution(rcvcmd_t* cmd, rcvcmd_solution_t* data);
/** @brief Discard errors under the server lock at the start of the error command. */
void rcvcmd_clear_error(rcvcmd_t* cmd);
/** @brief Preserve the unlocked discard before cyclic solution display until D-17. */
void rcvcmd_clear_solution(rcvcmd_t* cmd);

#ifdef __cplusplus
}
#endif

#endif /* RCVCMD_H */
