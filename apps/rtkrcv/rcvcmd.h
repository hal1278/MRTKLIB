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
#ifndef RCVCMD_H
#define RCVCMD_H

#include "mrtklib/mrtk_options.h"
#include "mrtklib/mrtk_rtksvr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The layer owns configuration, server start/stop, and process server/monitor
 * resources. Display reads still use temporary accessors below. Threading
 * remains the same as before the split; no configuration mutex is added. */
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

/* temporary until stage 3 */
rtksvr_t* rcvcmd_server(rcvcmd_t* cmd);
stream_t* rcvcmd_monitor(rcvcmd_t* cmd);
/* temporary until stage 3 */
int rcvcmd_solution_height(rcvcmd_t* cmd);

#ifdef __cplusplus
}
#endif

#endif /* RCVCMD_H */
