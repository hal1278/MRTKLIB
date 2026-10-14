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

/* Stage 1 owns configuration and process server/monitor resources. Server
 * start/stop and display reads still use temporary accessors below. Threading
 * remains the same as before the split; no configuration mutex is added. */
typedef struct rcvcmd rcvcmd_t;

/* temporary until stage 2 (solution height access until stage 3) */
typedef struct {
    int strtype[8];        /* stream types */
    char strpath[8][1024]; /* stream paths */
    int strfmt[5];         /* stream formats */
    int svrcycle;          /* server cycle (ms) */
    int timeout;           /* timeout time (ms) */
    int reconnect;         /* reconnect interval (ms) */
    int nmeacycle;         /* nmea request cycle (ms) */
    int buffsize;          /* input buffer size (bytes) */
    int navmsgsel;         /* navigation mesaage select */
    char proxyaddr[256];   /* http/ntrip proxy */
    int nmeareq;           /* nmea request type (0:off,1:lat/lon,2:single) */
    double nmeapos[3];     /* nmea position (lat/lon/height) (deg,m) */
    char rcvcmds[3][1024]; /* receiver commands files */
    char startcmd[1024];   /* start command */
    char stopcmd[1024];    /* stop command */
    int fswapmargin;       /* file swap margin (s) */
    char sta_name[256];    /* station name */
    prcopt_t prcopt;       /* processing options */
    solopt_t solopt[2];    /* solution options */
    filopt_t filopt;       /* file options */
    gtime_t rst;           /* raw/rtcm data start time */
} rcvcmd_settings_t;

typedef struct {
    opt_t* opt;
    int system;
    int index;
} rcvcmd_option_t;

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
/** @brief Clear modified flags at the existing point in server start. */
void rcvcmd_clear_modified(rcvcmd_t* cmd);
/** @brief Load system then receiver options, preserving partial changes on failure. */
int rcvcmd_load(rcvcmd_t* cmd, const char* file);
/** @brief Write receiver then system options with the original comment; return 1 on success. */
int rcvcmd_save(rcvcmd_t* cmd, const char* file);

/* temporary until stage 3 */
rtksvr_t* rcvcmd_server(rcvcmd_t* cmd);
stream_t* rcvcmd_monitor(rcvcmd_t* cmd);
/* temporary until stage 2 (solution height access until stage 3) */
rcvcmd_settings_t* rcvcmd_settings(rcvcmd_t* cmd);

#ifdef __cplusplus
}
#endif

#endif /* RCVCMD_H */
