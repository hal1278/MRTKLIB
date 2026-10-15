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
 * Each display has one reader filling caller-owned storage. Server values use
 * the existing server lock; stream fields use their own stream locks.
 * Solution/error readers use caller-owned cursors in every lifecycle state.
 * Events are collected once and copied under the publish mutex; adapters print
 * after unlock.
 * Readers allocate no large temporary
 * snapshots; callers keep one copy in their existing display stack frame.
 * Pointer members copied inside rtk_t are not live handles for callers to
 * dereference; displays consume only copied values. Stream snapshots contain
 * only the display fields, with no copied locks or port handles.
 *
 * A publish mutex protects lifecycle state, claims and counted readers.
 * Start/stop/restart exclude server/stream readers for the whole transition.
 * Those accepted while stopped retain the last values; refused reads zero
 * caller storage. Event readers remain available during transitions.
 * Never trace, take server/stream locks or call adapters under the publish mutex.
 * A configuration mutex serializes option operations and console settings.
 * Configuration -> publish lock is allowed; never wait for configuration while
 * holding the publish mutex. Start captures staged settings and clears modified
 * flags under configuration, then releases it and prepares a separate run copy.
 * Shutdown requests share the publish mutex; an active transition finishes
 * before final shutdown. Status, solution and log events share one sequence.
 * Stream formats use the server lock, separately from stream field copies.
 * The collector is joined at close, before trace teardown.
 */
#ifndef RCVCMD_H
#define RCVCMD_H

#include "mrtklib/mrtk_options.h"
#include "mrtklib/mrtk_rtksvr.h"
#include "rcvevent.h"

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
    RCVCMD_NOTICE_START_COMMAND_ERROR,
    RCVCMD_NOTICE_STOP_COMMAND_ERROR,
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

typedef enum {
    RCVCMD_OVERWRITE_REFUSE_STOP = 0,
    RCVCMD_OVERWRITE_ALLOW = 1,
    RCVCMD_OVERWRITE_REFUSE_CONTINUE = 2
} rcvcmd_overwrite_t;

typedef struct {
    void* arg;
    void (*notice)(void* arg, const rcvcmd_notice_t* notice);
    int (*confirm_overwrite)(void* arg, const char* path); /* rcvcmd_overwrite_t */
} rcvcmd_io_t;

typedef enum { RCVCMD_STOPPED, RCVCMD_STARTING, RCVCMD_RUNNING, RCVCMD_STOPPING } rcvcmd_state_t;

typedef enum { RCVCMD_STOP_NONE, RCVCMD_STOP_REQUEST, RCVCMD_STOP_ERROR } rcvcmd_stop_reason_t;

typedef enum {
    RCVCMD_START_ERROR_NONE,
    RCVCMD_START_ERROR_CORRECTION,
    RCVCMD_START_ERROR_OVERWRITE,
    RCVCMD_START_ERROR_SERVER,
    RCVCMD_START_ERROR_STDIN_RESERVED
} rcvcmd_start_error_t;

typedef struct {
    rcvcmd_state_t state;
    rcvcmd_stop_reason_t stop_reason;
    rcvcmd_start_error_t last_error;
    char paths[RCVEVENT_PATHS]; /* distinct refused paths, NUL-separated, double NUL at end */
} rcvcmd_lifecycle_t;

typedef enum {
    RCVCMD_SUCCESS,
    RCVCMD_ALREADY_RUNNING,
    RCVCMD_BUSY,
    RCVCMD_SHUTTING_DOWN,
    RCVCMD_START_FAILED
} rcvcmd_result_kind_t;

typedef struct {
    rcvcmd_result_kind_t kind;
    rcvcmd_state_t state; /* current state, including the transition for busy */
    uint64_t seq;         /* claim event, or current boundary for an unchanged request */
} rcvcmd_result_t;

typedef struct {
    uint64_t seq;
    rcvcmd_lifecycle_t lifecycle;
    int mode;            /* valid while running */
    int has_solution;    /* valid while running */
    rcvevent_t solution; /* last solution event of this run, including NONE */
} rcvcmd_snapshot_t;

typedef enum { RCVCMD_OP_START, RCVCMD_OP_STOP, RCVCMD_OP_RESTART, RCVCMD_OP_SHUTDOWN } rcvcmd_operation_t;

typedef struct {
    uint64_t seq; /* zero when no execution is required */
} rcvcmd_claim_t;

/** @brief Claim a lifecycle transition and publish its initial status.
 * @param[in,out] cmd The sole command-layer instance.
 * @param[in] operation Requested operation; SHUTDOWN waits for the preceding claim.
 * @param[out] claim Transferable execution token, zero for refusals or an unchanged stop.
 * @return Acceptance with claim-event seq, or refusal with the current boundary.
 * Waits only for counted readers to copy their data. A nonzero token MUST be
 * executed exactly once, from any thread, using rcvcmd_execute(). Until then
 * other lifecycle operations are busy and shutdown waits. Callbacks are not run.
 */
rcvcmd_result_t rcvcmd_claim(rcvcmd_t* cmd, rcvcmd_operation_t operation, rcvcmd_claim_t* claim);
/** @brief Execute a previously accepted claim, publishing its final status.
 * @param[in,out] cmd Instance that issued the claim.
 * @param[in] claim Nonzero token from rcvcmd_claim; consumed at entry.
 * @param[in] io Synchronous callbacks, borrowed until this call returns.
 * @return Final result with the original claim seq; BUSY for an already consumed token.
 * Holds no publish mutex while executing server operations or callbacks.
 */
rcvcmd_result_t rcvcmd_execute(rcvcmd_t* cmd, const rcvcmd_claim_t* claim, const rcvcmd_io_t* io);

/** @brief Return the last published sequence under the publish mutex. */
uint64_t rcvcmd_last_seq(rcvcmd_t* cmd);
/** @brief Copy sync/getStatus and its event boundary atomically. */
void rcvcmd_snapshot(rcvcmd_t* cmd, rcvcmd_snapshot_t* data);
/** @brief Copy events after a cursor; return -1 for a cursor beyond the boundary.
 * @param[out] out Caller storage for at least limit events.
 * @param[in] limit Maximum events to copy (> 0).
 * @param[out] res Count, next cursor, more and lost-event information.
 */
int rcvcmd_read_events(rcvcmd_t* cmd, uint64_t after, rcvevent_t* out, int limit, rcvevent_read_t* res);

/** @brief Copy lifecycle state, stop reason and structured last start error. */
void rcvcmd_get_lifecycle(rcvcmd_t* cmd, rcvcmd_lifecycle_t* data);
/** @brief Start with synchronous adapter callbacks; return kind and current state.
 * @param[in,out] cmd The sole command-layer instance.
 * @param[in] io Adapter output and overwrite callbacks; both must be supplied.
 * @return Success, already running, busy, shutting down or start failed.
 */
rcvcmd_result_t rcvcmd_start(rcvcmd_t* cmd, const rcvcmd_io_t* io);
/** @brief Stop; a stopped server succeeds without effect or notice.
 * @param[in,out] cmd The sole command-layer instance.
 * @param[in] io Adapter output and overwrite callbacks; both must be supplied.
 * @return Success, busy or shutting down.
 */
rcvcmd_result_t rcvcmd_stop(rcvcmd_t* cmd, const rcvcmd_io_t* io);
/** @brief Restart under one claim; when stopped, behave like start.
 * @param[in,out] cmd The sole command-layer instance.
 * @param[in] io Adapter output and overwrite callbacks; both must be supplied.
 * @return Success, busy, shutting down or start failed.
 */
rcvcmd_result_t rcvcmd_restart(rcvcmd_t* cmd, const rcvcmd_io_t* io);

/** @brief Record an idempotent shutdown request without waiting.
 * @param[in,out] cmd The sole command-layer instance.
 */
void rcvcmd_request_shutdown(rcvcmd_t* cmd);
/** @brief Read the shutdown request under the publish mutex.
 * @param[in] cmd The sole command-layer instance.
 * @return 1 if shutdown was requested, 0 otherwise.
 */
int rcvcmd_shutdown_requested(rcvcmd_t* cmd);
/** @brief Reserve stdin for the parent-liveness watcher, preventing file input from consuming it.
 * @param[in,out] cmd The sole command-layer instance.
 * Reservation is permanent for this instance and takes effect at the next start.
 */
void rcvcmd_reserve_stdin(rcvcmd_t* cmd);
/** @brief Wait for any transition, then stop a running server despite the request.
 * @param[in,out] cmd The sole command-layer instance.
 * @param[in] io Adapter output and overwrite callbacks; both must be supplied.
 */
void rcvcmd_shutdown(rcvcmd_t* cmd, const rcvcmd_io_t* io);

/** @brief Allocate resources, register the trace sink, then start the collector.
 * @param[in] ctx Context already assigned to g_mrtk_ctx, before other tracing threads start.
 * @return The sole instance, or NULL on failure or an existing instance. After a failure
 *         that follows the registration, the context keeps a disabled sink; use a new context.
 */
rcvcmd_t* rcvcmd_create(mrtk_ctx_t* ctx);
/** @brief Set the event log level, independently of the trace file level.
 * @param[in] cmd The sole command-layer instance.
 * @param[in] level 1 (trace errors and RTK/VRS warnings from errbuf) or 2 (adds trace warnings,
 *                  which also carry the errbuf texts).
 * @return 1 on success; 0 with no change for another level or no sink.
 */
int rcvcmd_set_log_level(rcvcmd_t* cmd, int level);
/** @brief Read the current event log level.
 * @param[in] cmd The sole command-layer instance.
 * @return 1 or 2, or 0 when built without a trace sink.
 */
int rcvcmd_log_level(rcvcmd_t* cmd);
/** @brief Free the server after the adapter has torn down its runtime context. */
void rcvcmd_destroy(rcvcmd_t* cmd);
/** @brief Initialize server/monitor, load startup options, navigation and status output.
 * @param[in,out] file Options path buffer (1024 bytes); filled with the default if empty.
 * @param[in] password Parsed -w value, applied before loading console-passwd from the file.
 */
void rcvcmd_initialize(rcvcmd_t* cmd, char* file, const char* password, const char* staname, int moniport, int outstat,
                       gtime_t rst);
/** @brief Join the collector, then close initialized monitor/status before runtime teardown. */
void rcvcmd_close(rcvcmd_t* cmd);
/** @brief Save navigation on normal shutdown, before trace/context teardown. */
void rcvcmd_save_navigation(rcvcmd_t* cmd);

/** @brief Copy the configured console password under the configuration mutex.
 * @param[in] cmd The sole command-layer instance.
 * @param[out] password Caller-owned storage.
 * @param[in] size Storage size including the terminating byte.
 */
void rcvcmd_password(rcvcmd_t* cmd, char* password, size_t size);
/** @brief Read the configured console time format. */
int rcvcmd_time_type(rcvcmd_t* cmd);
/** @brief Read the configured console solution format. */
int rcvcmd_solution_type(rcvcmd_t* cmd);
/** @brief Read the captured run's solution height mode for solution formatting. */
int rcvcmd_solution_height(rcvcmd_t* cmd);
/** @brief Read the configured console solution flags. */
int rcvcmd_solution_flags(rcvcmd_t* cmd);
/** @brief Read the requested monitor port. */
int rcvcmd_monitor_port(rcvcmd_t* cmd);

/** @brief Lock configuration to format/copy the complete option listing.
 * Release before adapter output, input, callbacks or external commands. */
void rcvcmd_lock_options(rcvcmd_t* cmd);
/** @brief Release configuration before printing the copied option listing. */
void rcvcmd_unlock_options(rcvcmd_t* cmd);
/** @brief Get the ordered option table while the listing lock is held.
 * system=0: receiver, system=1: core system. */
const opt_t* rcvcmd_options(rcvcmd_t* cmd, int system);
/** @brief Read a modified flag while the listing lock is held. */
int rcvcmd_option_modified(rcvcmd_t* cmd, int system, int index);
/** @brief Find a receiver option first, then a system option; return 1 if found. */
int rcvcmd_find_option(rcvcmd_t* cmd, const char* name, rcvcmd_option_t* option);
typedef enum { RCVCMD_CONFIG_SUCCESS, RCVCMD_CONFIG_ERROR, RCVCMD_CONFIG_REFUSED } rcvcmd_config_result_t;

/** @brief Return whether the option is non-console and needs the restart notice. */
int rcvcmd_option_requires_restart(const rcvcmd_option_t* option);
/** @brief Check whether set may prompt; application rechecks after input. */
int rcvcmd_can_set_option(rcvcmd_t* cmd, const rcvcmd_option_t* option);
/** @brief Apply and mark a value under configuration exclusion.
 * @return Success, invalid value or refusal because a non-console change needs stop.
 */
rcvcmd_config_result_t rcvcmd_set_option(rcvcmd_t* cmd, const rcvcmd_option_t* option, const char* value);
/** @brief Load options while stopped, preserving partial changes on failure.
 * @return Success, file error or refusal because changing options needs stop.
 */
rcvcmd_config_result_t rcvcmd_load(rcvcmd_t* cmd, const char* file);
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
    /* Only type, state, inb/inr, outb/outr, path and msg are copied. Other
     * stream_t members are not snapshot data and must not be used. Keeping
     * this storage type avoids coupling the lock fix to adapter changes. */
    stream_t stream[9];
    int format[9];
} rcvcmd_stream_t;

typedef struct {
    char errbuf[MAXERRMSG + 1];
    int n;
} rcvcmd_error_t;

typedef struct {
    rcvevent_t events[MAXSOLBUF];
    int nsol;
} rcvcmd_solution_t;

/** @brief Copy newest solutions of the latest run after cursor, oldest first, in any state.
 * Solutions before the latest starting event are skipped, including during that start.
 * @param[in,out] cursor Advanced to the last published sequence.
 * @param[out] data Caller storage for at most MAXSOLBUF solution events.
 * @return Number of solutions copied.
 */
int rcvcmd_read_solutions(rcvcmd_t* cmd, uint64_t* cursor, rcvcmd_solution_t* data);
/** @brief Copy log texts with newlines in any lifecycle state.
 * At log level 1, select errbuf-origin entries only; at level 2, select all logs.
 * Dropped events are skipped. Stop before a text that will not fit.
 * @param[in,out] cursor Advanced through the last examined event.
 * @param[out] data Caller storage with MAXERRMSG + 1 text bytes.
 * @return Number of text bytes copied, excluding the terminating NUL.
 */
int rcvcmd_read_errors(rcvcmd_t* cmd, uint64_t* cursor, rcvcmd_error_t* data);

/* Server/stream readers below return 1 when accepted, 0 during a claim/transition.
 * Refused readers zero all caller storage. */

/** @brief Copy status fields and the existing deep copies of vector values.
 * @param[in] cmd The sole command-layer instance.
 * @param[out] data Caller-owned storage, filled directly without another snapshot copy.
 */
int rcvcmd_read_status(rcvcmd_t* cmd, rcvcmd_status_t* data);
/** @brief Copy the satellite display's RTK values.
 * @param[in] cmd The sole command-layer instance.
 * @param[out] data Caller-owned storage, filled directly without another snapshot copy.
 */
int rcvcmd_read_satellite(rcvcmd_t* cmd, rcvcmd_satellite_t* data);
/** @brief Copy rover then base observations matching sys, with the existing cap.
 * @param[in] cmd The sole command-layer instance.
 * @param[in] sys Satellite-system mask, with the original observation filtering.
 * @param[out] data Caller-owned storage, filled directly without another snapshot copy.
 */
int rcvcmd_read_observ(rcvcmd_t* cmd, int sys, rcvcmd_observ_t* data);
/** @brief Copy navigation time, ephemerides, GPS ionosphere and UTC parameters.
 * @param[in] cmd The sole command-layer instance.
 * @param[out] data Caller-owned storage, filled directly without another snapshot copy.
 */
int rcvcmd_read_navidata(rcvcmd_t* cmd, rcvcmd_navidata_t* data);
/** @brief Copy solution time and channel-zero SSR values.
 * @param[in] cmd The sole command-layer instance.
 * @param[out] data Caller-owned storage, filled directly without another snapshot copy.
 */
int rcvcmd_read_ssr(rcvcmd_t* cmd, rcvcmd_ssr_t* data);
/** @brief Copy formats under the server lock and displayed fields under each stream lock.
 * @param[in] cmd The sole command-layer instance.
 * @param[out] data Caller-owned storage, filled directly without another snapshot copy.
 */
int rcvcmd_read_stream(rcvcmd_t* cmd, rcvcmd_stream_t* data);

#ifdef __cplusplus
}
#endif

#endif /* RCVCMD_H */
