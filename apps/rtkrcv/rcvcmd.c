/*------------------------------------------------------------------------------
 * rcvcmd.c : rtk receiver command layer
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
#include "rcvcmd.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/wait.h>
#endif

#include "mrtklib/mrtk_antenna.h"
#include "mrtklib/mrtk_const.h"
#include "mrtklib/mrtk_coords.h"
#include "mrtklib/mrtk_geoid.h"
#include "mrtklib/mrtk_mat.h"
#include "mrtklib/mrtk_station.h"
#include "mrtklib/mrtk_sys.h"
#include "mrtklib/mrtk_trace.h"
#include "mrtklib/mrtklib.h"

#define NAVIFILE "rtkrcv.nav"             /* navigation save file */
#define STATFILE "rtkrcv_%Y%m%d%h%M.stat" /* solution status file */
#define INTKEEPALIVE 1000                 /* keep alive interval (ms) */
#define PRGNAME "rtkrcv"                  /* program name */
#define OPTSDIR "."                       /* default config directory */
#define OPTSFILE "rtkrcv.conf"            /* default config file */
#define MAXSTR 1024                       /* max length of a stream */

/* >= 10 * MAXSOLBUF plus interleaved logs; measure in the later trace-sink unit (D-35). */
#define RCVCMD_EVENT_CAPACITY 4096
#define RCVCMD_COLLECT_MS 100

_Static_assert(sizeof(rcvevent_status_t) <= sizeof(rcvevent_log_t), "event paths must fit inside the log member");
_Static_assert(sizeof(rcvevent_t) <= 536, "event paths must not enlarge ring entries");

typedef struct {
    sol_t solutions[MAXSOLBUF];
    rcvevent_t events[MAXSOLBUF];
    double rb[3];
    char errors[MAXERRMSG];
    int nsol, neb;
} rcvcmd_staging_t;

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

struct rcvcmd {
    pthread_mutex_t config_lock;
    pthread_mutex_t publish_lock;
    pthread_cond_t publish_cond;
    rcvcmd_lifecycle_t lifecycle;
    int claimed, executing;
    uint64_t claim_seq;
    rcvcmd_operation_t operation;
    int readers;
    int shutdown_requested;
    int stdin_reserved; /* configuration mutex; captured with each start's settings */
    rcvevent_ring_t events;
    mrtk_ctx_t* ctx;
    int log_level;
    rcvcmd_staging_t* staging;
    pthread_t collector;
    int collector_started, collector_exit, initialized;
    int run_mode, run_geoid;
    uint64_t solution_start_seq;
    rcvevent_t last_solution;
    int has_solution, previous_none;
    gtime_t none_time;
    rtksvr_t* svr;     /* rtk server struct */
    stream_t moni;     /* monitor stream */
    char passwd[1024]; /* login password */
    int timetype;      /* time format (0:gpst,1:utc,2:jst,3:tow) */
    int soltype;       /* sol format (0:dms,1:deg,2:xyz,3:enu,4:pyl) */
    int solflag;       /* sol flag (1:std+2:age/ratio/ns) */
    rcvcmd_settings_t settings;
    rcvcmd_settings_t run_settings;
    int modflgr[256]; /* modified flags of receiver options */
    int modflgs[256]; /* modified flags of system options */
    int moniport;     /* monitor port */
    int keepalive;    /* keep alive flag */
    int outstat;      /* solution status output level */
    opt_t rcvopts[43];
};

/* receiver options table ----------------------------------------------------*/
#define TIMOPT "0:gpst,1:utc,2:jst,3:tow"
#define CONOPT "0:dms,1:deg,2:xyz,3:enu,4:pyl"
#define FLGOPT "0:off,1:std+2:age/ratio/ns"
#define ISTOPT "0:off,1:serial,2:file,3:tcpsvr,4:tcpcli,6:ntripcli,7:ftp,8:http,10:udpsvr,11:udpcli"
#define OSTOPT "0:off,1:serial,2:file,3:tcpsvr,4:tcpcli,5:ntripsvr,9:ntripcas,10:udpsvr,11:udpcli"
#define FMTOPT                                                                                                     \
    "0:rtcm2,1:rtcm3,2:oem4,3:oem3,4:ubx,5:ss2,6:hemis,7:skytraq,8:javad,9:nvs,10:binex,11:rt17,12:sbf,14:sp3,20:" \
    "stat,21:l6e,22:clas"
#define NMEOPT "0:off,1:latlon,2:single"
#define SOLOPT "0:llh,1:xyz,2:enu,3:nmea,4:stat"
#define MSGOPT "0:all,1:rover,2:base,3:corr"

/* Core sysopts points at process-global buffers in mrtk_options.c, so a
 * second instance would share configuration. Keep one static instance and
 * its option addresses; callers use a handle so only this layer must change
 * if the core removes those globals. */
static rcvcmd_t instance = {
    .passwd = "admin",
    .solflag = 2,
    .settings =
        {
            .strtype = {STR_SERIAL, STR_NONE, STR_NONE, STR_NONE, STR_NONE, STR_NONE, STR_NONE, STR_NONE},
            .strfmt = {STRFMT_UBX, STRFMT_RTCM3, STRFMT_SP3, SOLF_LLH, SOLF_NMEA},
            .svrcycle = 10,
            .timeout = 10000,
            .reconnect = 10000,
            .nmeacycle = 5000,
            .buffsize = 32768,
            .fswapmargin = 30,
        },
    .rcvopts = {{"console-passwd", 2, (void*)instance.passwd, ""},
                {"console-timetype", 3, (void*)&instance.timetype, TIMOPT},
                {"console-soltype", 3, (void*)&instance.soltype, CONOPT},
                {"console-solflag", 0, (void*)&instance.solflag, FLGOPT},

                {"inpstr1-type", 3, (void*)&instance.settings.strtype[0], ISTOPT},
                {"inpstr2-type", 3, (void*)&instance.settings.strtype[1], ISTOPT},
                {"inpstr3-type", 3, (void*)&instance.settings.strtype[2], ISTOPT},
                {"inpstr1-path", 2, (void*)instance.settings.strpath[0], ""},
                {"inpstr2-path", 2, (void*)instance.settings.strpath[1], ""},
                {"inpstr3-path", 2, (void*)instance.settings.strpath[2], ""},
                {"inpstr1-format", 3, (void*)&instance.settings.strfmt[0], FMTOPT},
                {"inpstr2-format", 3, (void*)&instance.settings.strfmt[1], FMTOPT},
                {"inpstr3-format", 3, (void*)&instance.settings.strfmt[2], FMTOPT},
                {"inpstr2-nmeareq", 3, (void*)&instance.settings.nmeareq, NMEOPT},
                {"inpstr2-nmealat", 1, (void*)&instance.settings.nmeapos[0], "deg"},
                {"inpstr2-nmealon", 1, (void*)&instance.settings.nmeapos[1], "deg"},
                {"inpstr2-nmeahgt", 1, (void*)&instance.settings.nmeapos[2], "m"},
                {"outstr1-type", 3, (void*)&instance.settings.strtype[3], OSTOPT},
                {"outstr2-type", 3, (void*)&instance.settings.strtype[4], OSTOPT},
                {"outstr1-path", 2, (void*)instance.settings.strpath[3], ""},
                {"outstr2-path", 2, (void*)instance.settings.strpath[4], ""},
                {"outstr1-format", 3, (void*)&instance.settings.strfmt[3], SOLOPT},
                {"outstr2-format", 3, (void*)&instance.settings.strfmt[4], SOLOPT},
                {"logstr1-type", 3, (void*)&instance.settings.strtype[5], OSTOPT},
                {"logstr2-type", 3, (void*)&instance.settings.strtype[6], OSTOPT},
                {"logstr3-type", 3, (void*)&instance.settings.strtype[7], OSTOPT},
                {"logstr1-path", 2, (void*)instance.settings.strpath[5], ""},
                {"logstr2-path", 2, (void*)instance.settings.strpath[6], ""},
                {"logstr3-path", 2, (void*)instance.settings.strpath[7], ""},

                {"misc-svrcycle", 0, (void*)&instance.settings.svrcycle, "ms"},
                {"misc-timeout", 0, (void*)&instance.settings.timeout, "ms"},
                {"misc-reconnect", 0, (void*)&instance.settings.reconnect, "ms"},
                {"misc-nmeacycle", 0, (void*)&instance.settings.nmeacycle, "ms"},
                {"misc-buffsize", 0, (void*)&instance.settings.buffsize, "bytes"},
                {"misc-navmsgsel", 3, (void*)&instance.settings.navmsgsel, MSGOPT},
                {"misc-proxyaddr", 2, (void*)instance.settings.proxyaddr, ""},
                {"misc-fswapmargin", 0, (void*)&instance.settings.fswapmargin, "s"},

                {"misc-startcmd", 2, (void*)instance.settings.startcmd, ""},
                {"misc-stopcmd", 2, (void*)instance.settings.stopcmd, ""},

                {"file-cmdfile1", 2, (void*)instance.settings.rcvcmds[0], ""},
                {"file-cmdfile2", 2, (void*)instance.settings.rcvcmds[1], ""},
                {"file-cmdfile3", 2, (void*)instance.settings.rcvcmds[2], ""},

                {"", 0, NULL, ""}},
};
/* These helpers publish only copies while the publish mutex is held. */
static uint64_t publish_status(rcvcmd_t* cmd) {
    rcvevent_t ev = {0};
    ev.kind = RCVEVENT_STATUS;
    ev.data.status.state = cmd->lifecycle.state;
    ev.data.status.stop_reason = cmd->lifecycle.stop_reason;
    ev.data.status.last_error = cmd->lifecycle.last_error;
    memcpy(ev.data.status.paths, cmd->lifecycle.paths, sizeof(ev.data.status.paths));
    return rcvevent_publish(&cmd->events, &ev);
}

static void clear_run_solution(rcvcmd_t* cmd) {
    cmd->has_solution = cmd->previous_none = 0;
    memset(&cmd->last_solution, 0, sizeof(cmd->last_solution));
    memset(&cmd->none_time, 0, sizeof(cmd->none_time));
}

/* Core stream errors may contain user:password@host (including @ in a password). */
static void mask_stream_error(const char* text, char* out, size_t size) {
    const char* start = strstr(text, "path=");
    const char *at, *colon;
    start = start ? start + 5 : text;
    at = strrchr(start, '@');
    colon = strchr(start, ':');
    if (at && colon && colon < at)
        snprintf(out, size, "%.*s***%s", (int)(colon + 1 - text), text, at);
    else
        snprintf(out, size, "%s", text);
}

#ifdef TRACE
/* svr->lock / stream locks / config_lock -> publish_lock (innermost).
 * Never trace or take another lock under publish_lock. The core passes up to
 * MRTK_TRACE_SINK_MSGLEN - 1 bytes; RCVEVENT_TEXT is the only cut of log text. */
static void publish_trace(void* arg, int level, const char* msg) {
    rcvcmd_t* cmd = (rcvcmd_t*)arg;
    rcvevent_t ev = {0};
    char safe[MRTK_TRACE_SINK_MSGLEN];
    size_t n;
    mask_stream_error(msg, safe, sizeof(safe));
    msg = safe;
    n = strlen(msg);
    while (n && (msg[n - 1] == '\n' || msg[n - 1] == '\r')) n--;
    if (!n) return;
    if (n >= RCVEVENT_TEXT) n = RCVEVENT_TEXT - 1;
    ev.kind = RCVEVENT_LOG;
    ev.data.log.level = level;
    ev.data.log.origin = RCVEVENT_LOG_TRACE;
    memcpy(ev.data.log.text, msg, n);
    pthread_mutex_lock(&cmd->publish_lock);
    rcvevent_publish(&cmd->events, &ev);
    pthread_mutex_unlock(&cmd->publish_lock);
}
#endif

/* The counted collector or the transition claim exclusively owns staging. */
static void collect_events(rcvcmd_t* cmd) {
    rcvcmd_staging_t* stage = cmd->staging;
    rtksvr_t* svr = cmd->svr;
    int i, mode, geoid, uses_base;
    double pos[3];

    pthread_mutex_lock(&cmd->publish_lock);
    mode = cmd->run_mode;
    geoid = cmd->run_geoid;
    pthread_mutex_unlock(&cmd->publish_lock);
    uses_base = mode == PMODE_DGPS || mode == PMODE_KINEMA || mode == PMODE_STATIC || mode == PMODE_MOVEB ||
                mode == PMODE_FIXED || mode == PMODE_VRS_RTK;

    rtksvrlock(svr);
    stage->nsol = svr->nsol;
    memcpy(stage->solutions, svr->solbuf, stage->nsol * sizeof(sol_t));
    memcpy(stage->rb, svr->rtk.rb, sizeof(stage->rb));
    stage->neb = svr->rtk.neb;
    memcpy(stage->errors, svr->rtk.errbuf, stage->neb);
    svr->nsol = svr->rtk.neb = 0;
    rtksvrunlock(svr);

    for (i = 0; i < stage->nsol; i++) {
        rcvevent_t* ev = &stage->events[i];
        rcvevent_solution_t* solution = &ev->data.solution;
        sol_t* sol = &stage->solutions[i];
        memset(ev, 0, sizeof(*ev));
        ev->kind = RCVEVENT_SOLUTION;
        solution->leap = timediff(sol->time, gpst2utc(sol->time));
        if (sol->stat == SOLQ_NONE) {
            solution->sol.time = sol->time;
            solution->sol.stat = sol->stat;
            continue;
        }
        solution->sol = *sol;
        memcpy(solution->rb, stage->rb, sizeof(solution->rb));
        solution->has_base = uses_base && norm(stage->rb, 3) > 0.0;
        solution->geoid_model = geoid;
        if (geoid == 0 && isfinite(sol->rr[0]) && isfinite(sol->rr[1]) && isfinite(sol->rr[2])) {
            ecef2pos(sol->rr, pos);
            if (!isfinite(pos[0]) || !isfinite(pos[1]) || !isfinite(pos[2])) continue;
            solution->separation = geoidh(pos);
            solution->has_separation = solution->separation != 0.0;
        }
    }
}

static void publish_collected(rcvcmd_t* cmd) {
    rcvcmd_staging_t* stage = cmd->staging;
    int i, first;
    for (i = 0; i < stage->nsol; i++) {
        rcvevent_t* ev = &stage->events[i];
        gtime_t time = ev->data.solution.sol.time;
        int none = ev->data.solution.sol.stat == SOLQ_NONE;
        if (none && (time.time == 0 ||
                     (cmd->previous_none && time.time == cmd->none_time.time && time.sec == cmd->none_time.sec))) {
            continue;
        }
        ev->seq = rcvevent_publish(&cmd->events, ev);
        cmd->last_solution = *ev;
        cmd->has_solution = 1;
        cmd->previous_none = none;
        cmd->none_time = time;
    }
    for (first = 0; cmd->log_level != 2 && first < stage->neb; first = i + 1) {
        rcvevent_t ev = {0};
        size_t n;
        for (i = first; i < stage->neb && stage->errors[i] != '\n'; i++) {
        }
        if (i == first) continue;
        n = (size_t)(i - first);
        if (n >= RCVEVENT_TEXT) n = RCVEVENT_TEXT - 1;
        ev.kind = RCVEVENT_LOG;
        ev.data.log.level = 1;
        ev.data.log.origin = RCVEVENT_LOG_ERRBUF;
        memcpy(ev.data.log.text, stage->errors + first, n);
        rcvevent_publish(&cmd->events, &ev);
    }
}

static void* collect_thread(void* arg) {
    rcvcmd_t* cmd = (rcvcmd_t*)arg;
    for (;;) {
        sleepms(RCVCMD_COLLECT_MS);
        pthread_mutex_lock(&cmd->publish_lock);
        if (cmd->collector_exit) {
            pthread_mutex_unlock(&cmd->publish_lock);
            return NULL;
        }
        if (cmd->claimed || cmd->lifecycle.state != RCVCMD_RUNNING) {
            pthread_mutex_unlock(&cmd->publish_lock);
            continue;
        }
        cmd->readers++;
        pthread_mutex_unlock(&cmd->publish_lock);
        collect_events(cmd);
        pthread_mutex_lock(&cmd->publish_lock);
        publish_collected(cmd);
        if (--cmd->readers == 0) pthread_cond_broadcast(&cmd->publish_cond);
        pthread_mutex_unlock(&cmd->publish_lock);
    }
}

/* thread to send keep alive for monitor port --------------------------------*/
static void* sendkeepalive(void* arg) {
    rcvcmd_t* cmd = (rcvcmd_t*)arg;
    trace(NULL, 3, "sendkeepalive: start\n");

    while (cmd->keepalive) {
        strwrite(&cmd->moni, (uint8_t*)"\r", 1);
        sleepms(INTKEEPALIVE);
    }
    trace(NULL, 3, "sendkeepalive: stop\n");
    return NULL;
}
/* open monitor port ---------------------------------------------------------*/
static int openmoni(rcvcmd_t* cmd, int port) {
    pthread_t thread;
    char path[64];

    trace(NULL, 3, "openmomi: port=%d\n", port);

    sprintf(path, ":%d", port);
    if (!stropen(&cmd->moni, STR_TCPSVR, STR_MODE_RW, path)) {
        return 0;
    }
    strsettimeout(&cmd->moni, cmd->settings.timeout, cmd->settings.reconnect);
    cmd->keepalive = 1;
    pthread_create(&thread, NULL, sendkeepalive, cmd);
    return 1;
}
/* close monitor port --------------------------------------------------------*/
static void closemoni(rcvcmd_t* cmd) {
    trace(NULL, 3, "closemoni:\n");
    cmd->keepalive = 0;

    /* send disconnect message */
    strwrite(&cmd->moni, (uint8_t*)MSG_DISCONN, strlen(MSG_DISCONN));

    /* wait fin from clients */
    sleepms(1000);

    strclose(&cmd->moni);
}
/* allocate server -----------------------------------------------------------*/
rcvcmd_t* rcvcmd_create(mrtk_ctx_t* ctx) {
    if (instance.svr) {
        return NULL;
    }
    /* heap-allocate rtksvr_t (~51 MB) to avoid BSS bloat */
    instance.svr = (rtksvr_t*)calloc(1, sizeof(rtksvr_t));
    if (!instance.svr) {
        fprintf(stderr, "error: rtksvr_t allocation failed\n");
        return NULL;
    }
    if (pthread_mutex_init(&instance.publish_lock, NULL)) {
        free(instance.svr);
        instance.svr = NULL;
        return NULL;
    }
    if (pthread_cond_init(&instance.publish_cond, NULL)) {
        pthread_mutex_destroy(&instance.publish_lock);
        free(instance.svr);
        instance.svr = NULL;
        return NULL;
    }
    if (pthread_mutex_init(&instance.config_lock, NULL)) {
        pthread_cond_destroy(&instance.publish_cond);
        pthread_mutex_destroy(&instance.publish_lock);
        free(instance.svr);
        instance.svr = NULL;
        return NULL;
    }
    instance.staging = (rcvcmd_staging_t*)calloc(1, sizeof(rcvcmd_staging_t));
    if (!instance.staging || !rcvevent_init(&instance.events, RCVCMD_EVENT_CAPACITY)) {
        free(instance.staging);
        instance.staging = NULL;
        pthread_mutex_destroy(&instance.config_lock);
        pthread_cond_destroy(&instance.publish_cond);
        pthread_mutex_destroy(&instance.publish_lock);
        free(instance.svr);
        instance.svr = NULL;
        return NULL;
    }
    instance.initialized = instance.collector_started = instance.collector_exit = 0;
    instance.solution_start_seq = 0;
    clear_run_solution(&instance);
    memset(&instance.lifecycle, 0, sizeof(instance.lifecycle));
    instance.claimed = instance.readers = instance.shutdown_requested = 0;
    instance.ctx = ctx;
    instance.log_level = 0;
#ifdef TRACE
    if (!tracesink(ctx, publish_trace, &instance, 2) || !rcvcmd_set_log_level(&instance, 1)) {
        rcvcmd_destroy(&instance);
        return NULL;
    }
#endif
    if (pthread_create(&instance.collector, NULL, collect_thread, &instance)) {
        fprintf(stderr, "event collector thread create error\n");
#ifdef TRACE
        /* A sink cannot be unregistered: disable it before its ring is freed. */
        tracesinklevel(ctx, 0);
#endif
        rcvcmd_destroy(&instance);
        return NULL;
    }
    instance.collector_started = 1;
    return &instance;
}

int rcvcmd_set_log_level(rcvcmd_t* cmd, int level) {
    int result = 0;
    if (level != 1 && level != 2) return 0;
    pthread_mutex_lock(&cmd->publish_lock);
    if (tracesinklevel(cmd->ctx, level)) {
        cmd->log_level = level;
        result = 1;
    }
    pthread_mutex_unlock(&cmd->publish_lock);
    return result;
}

int rcvcmd_log_level(rcvcmd_t* cmd) {
    int level;
    pthread_mutex_lock(&cmd->publish_lock);
    level = cmd->log_level;
    pthread_mutex_unlock(&cmd->publish_lock);
    return level;
}

void rcvcmd_destroy(rcvcmd_t* cmd) {
    if (!cmd) return;
    rcvcmd_close(cmd);
    rcvevent_free(&cmd->events);
    free(cmd->staging);
    cmd->staging = NULL;
    pthread_mutex_destroy(&cmd->config_lock);
    pthread_cond_destroy(&cmd->publish_cond);
    pthread_mutex_destroy(&cmd->publish_lock);
    free(cmd->svr);
    cmd->svr = NULL;
}

void rcvcmd_initialize(rcvcmd_t* cmd, char* file, const char* password, const char* staname, int moniport, int outstat,
                       gtime_t rst) {
    strcpy(cmd->passwd, password);
    cmd->moniport = moniport;
    cmd->outstat = outstat;
    cmd->settings.rst = rst;
    /* initialize rtk server and monitor port */
    rtksvrinit(cmd->svr);
    strinit(&cmd->moni);
    cmd->initialized = 1;

    /* load options file */
    if (!*file) {
        sprintf(file, "%s/%s", OPTSDIR, OPTSFILE);
    }

    resetsysopts();
    if (!loadopts(file, cmd->rcvopts) || !loadopts(file, sysopts)) {
        fprintf(stderr, "no options file: %s. defaults used\n", file);
    }
    getsysopts(&cmd->settings.prcopt, cmd->settings.solopt, &cmd->settings.filopt);

    if (strlen(cmd->svr->nav.biapath) == 0 && strlen(cmd->settings.filopt.bia) > 0) {
        strcpy(cmd->svr->nav.biapath, cmd->settings.filopt.bia);
    }
    if (strlen(cmd->svr->nav.fcbpath) == 0 && strlen(cmd->settings.filopt.fcb) > 0) {
        strcpy(cmd->svr->nav.fcbpath, cmd->settings.filopt.fcb);
    }
    if (strlen(staname) > 0) {
        strncpy(cmd->settings.prcopt.staname, staname, sizeof(cmd->settings.prcopt.staname));
    }
    /* read navigation data */
    if (!readnav(NAVIFILE, &cmd->svr->nav)) {
        fprintf(stderr, "no navigation data: %s\n", NAVIFILE);
    }
    if (outstat > 0) {
        rtkopenstat(STATFILE, outstat);
    }
    /* open monitor port */
    if (cmd->moniport > 0 && !openmoni(cmd, cmd->moniport)) {
        fprintf(stderr, "monitor port open error: %d\n", cmd->moniport);
    }
}

void rcvcmd_close(rcvcmd_t* cmd) {
    if (!cmd) return;
    pthread_mutex_lock(&cmd->publish_lock);
    cmd->collector_exit = 1;
    pthread_mutex_unlock(&cmd->publish_lock);
    if (cmd->collector_started) {
        pthread_join(cmd->collector, NULL);
        cmd->collector_started = 0;
    }
    if (!cmd->initialized) return;
    cmd->initialized = 0;
    if (cmd->moniport > 0) {
        closemoni(cmd);
    }
    if (cmd->outstat > 0) {
        rtkclosestat();
    }
}

void rcvcmd_save_navigation(rcvcmd_t* cmd) {
    /* save navigation data */
    if (!savenav(NAVIFILE, &cmd->svr->nav)) {
        fprintf(stderr, "navigation data save error: %s\n", NAVIFILE);
    }
}

void rcvcmd_password(rcvcmd_t* cmd, char* password, size_t size) {
    pthread_mutex_lock(&cmd->config_lock);
    snprintf(password, size, "%s", cmd->passwd);
    pthread_mutex_unlock(&cmd->config_lock);
}

static int read_console_setting(rcvcmd_t* cmd, const int* setting) {
    int value;
    pthread_mutex_lock(&cmd->config_lock);
    value = *setting;
    pthread_mutex_unlock(&cmd->config_lock);
    return value;
}

int rcvcmd_time_type(rcvcmd_t* cmd) { return read_console_setting(cmd, &cmd->timetype); }
int rcvcmd_solution_type(rcvcmd_t* cmd) { return read_console_setting(cmd, &cmd->soltype); }
int rcvcmd_solution_height(rcvcmd_t* cmd) { return read_console_setting(cmd, &cmd->run_settings.solopt[0].height); }
int rcvcmd_solution_flags(rcvcmd_t* cmd) { return read_console_setting(cmd, &cmd->solflag); }
int rcvcmd_monitor_port(rcvcmd_t* cmd) { return cmd->moniport; }

void rcvcmd_lock_options(rcvcmd_t* cmd) { pthread_mutex_lock(&cmd->config_lock); }
void rcvcmd_unlock_options(rcvcmd_t* cmd) { pthread_mutex_unlock(&cmd->config_lock); }

const opt_t* rcvcmd_options(rcvcmd_t* cmd, int system) { return system ? sysopts : cmd->rcvopts; }
int rcvcmd_option_modified(rcvcmd_t* cmd, int system, int index) {
    return system ? cmd->modflgs[index] : cmd->modflgr[index];
}

int rcvcmd_find_option(rcvcmd_t* cmd, const char* name, rcvcmd_option_t* option) {
    opt_t* opt;

    if ((opt = searchopt(name, cmd->rcvopts))) {
        option->system = 0;
        option->index = (int)(opt - cmd->rcvopts);
    } else if ((opt = searchopt(name, sysopts))) {
        option->system = 1;
        option->index = (int)(opt - sysopts);
    } else {
        return 0;
    }
    option->opt = opt;
    return 1;
}

static int configuration_stopped(rcvcmd_t* cmd) {
    int stopped;
    pthread_mutex_lock(&cmd->publish_lock);
    stopped = cmd->lifecycle.state == RCVCMD_STOPPED && !cmd->claimed;
    pthread_mutex_unlock(&cmd->publish_lock);
    return stopped;
}

int rcvcmd_option_requires_restart(const rcvcmd_option_t* option) {
    return strncmp(option->opt->name, "console-", 8) != 0;
}

int rcvcmd_can_set_option(rcvcmd_t* cmd, const rcvcmd_option_t* option) {
    return !rcvcmd_option_requires_restart(option) || configuration_stopped(cmd);
}

static int begin_configuration_change(rcvcmd_t* cmd) {
    if (!configuration_stopped(cmd)) {
        return 0;
    }
    pthread_mutex_lock(&cmd->config_lock);
    if (!configuration_stopped(cmd)) {
        pthread_mutex_unlock(&cmd->config_lock);
        return 0;
    }
    return 1;
}

rcvcmd_config_result_t rcvcmd_set_option(rcvcmd_t* cmd, const rcvcmd_option_t* option, const char* value) {
    rcvcmd_config_result_t result = RCVCMD_CONFIG_ERROR;
    int restart = rcvcmd_option_requires_restart(option);

    if (restart) {
        if (!begin_configuration_change(cmd)) {
            return RCVCMD_CONFIG_REFUSED;
        }
    } else {
        pthread_mutex_lock(&cmd->config_lock);
    }
    if (str2opt(option->opt, value)) {
        if (option->system) {
            getsysopts(&cmd->settings.prcopt, cmd->settings.solopt, &cmd->settings.filopt);
        }
        if (restart) {
            if (option->system) {
                cmd->modflgs[option->index] = 1;
            } else {
                cmd->modflgr[option->index] = 1;
            }
        }
        result = RCVCMD_CONFIG_SUCCESS;
    }
    pthread_mutex_unlock(&cmd->config_lock);
    return result;
}

static void rcvcmd_clear_modified(rcvcmd_t* cmd) {
    int i;
    for (i = 0; *cmd->rcvopts[i].name; i++) {
        cmd->modflgr[i] = 0;
    }
    for (i = 0; *sysopts[i].name; i++) {
        cmd->modflgs[i] = 0;
    }
}

rcvcmd_config_result_t rcvcmd_load(rcvcmd_t* cmd, const char* file) {
    rcvcmd_config_result_t result = RCVCMD_CONFIG_ERROR;

    if (!begin_configuration_change(cmd)) {
        return RCVCMD_CONFIG_REFUSED;
    }
    resetsysopts();
    if (loadopts(file, sysopts)) {
        getsysopts(&cmd->settings.prcopt, cmd->settings.solopt, &cmd->settings.filopt);
        if (loadopts(file, cmd->rcvopts)) {
            result = RCVCMD_CONFIG_SUCCESS;
        }
    }
    pthread_mutex_unlock(&cmd->config_lock);
    return result;
}

int rcvcmd_save(rcvcmd_t* cmd, const char* file) {
    char comment[256], s[64];
    int saved;
    time2str(utc2gpst(timeget()), s, 0);
    sprintf(comment, "%s options (%s, %s ver.%s)", PRGNAME, s, MRTKLIB_SOFTNAME, MRTKLIB_VERSION_STRING);
    pthread_mutex_lock(&cmd->config_lock);
    setsysopts(&cmd->settings.prcopt, cmd->settings.solopt, &cmd->settings.filopt);
    saved = saveopts(file, "w", comment, cmd->rcvopts) && saveopts(file, "a", NULL, sysopts);
    pthread_mutex_unlock(&cmd->config_lock);
    return saved;
}

/* adapter notice ------------------------------------------------------------*/
/* POSIX wait status / Windows exit code; the later platform layer takes this over. */
static void command_error(char* text, size_t size, const char* option, int status) {
    if (status == -1) {
        snprintf(text, size, "%s could not be run", option);
#ifdef _WIN32
    } else {
        snprintf(text, size, "%s failed (exit code %d)", option, status);
#else
    } else if (WIFEXITED(status)) {
        snprintf(text, size, "%s failed (exit code %d)", option, WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        snprintf(text, size, "%s failed (signal %d)", option, WTERMSIG(status));
    } else {
        snprintf(text, size, "%s failed", option);
#endif
    }
}

static void notice(const rcvcmd_io_t* io, rcvcmd_notice_code_t code, const char* path, const char* text, int status) {
    rcvcmd_notice_t value = {code, path, text, status};
    char safe[2048];
    char line[MRTK_TRACE_SINK_MSGLEN];
    switch (code) {
        case RCVCMD_NOTICE_CORRECTION_ERROR:
            snprintf(line, sizeof(line), "start error: %s", text);
            break;
        case RCVCMD_NOTICE_NO_COMMAND_FILE:
            snprintf(line, sizeof(line), "no command file: %s", path);
            break;
        case RCVCMD_NOTICE_NO_ANTENNA:
            snprintf(line, sizeof(line), "no antenna %s in %s", text, path);
            break;
        case RCVCMD_NOTICE_ANTENNA_FILE_ERROR:
            snprintf(line, sizeof(line), "antenna file open error: %s", path);
            break;
        case RCVCMD_NOTICE_GEOID_FILE_ERROR:
            snprintf(line, sizeof(line), "geoid data open error: %s", path);
            break;
        case RCVCMD_NOTICE_START_COMMAND_ERROR:
            command_error(line, sizeof(line), "misc-startcmd", status);
            break;
        case RCVCMD_NOTICE_STOP_COMMAND_ERROR:
            command_error(line, sizeof(line), "misc-stopcmd", status);
            break;
        case RCVCMD_NOTICE_SERVER_START_ERROR:
            mask_stream_error(text, safe, sizeof(safe));
            snprintf(line, sizeof(line), "rtk server start error (%s)", safe);
            break;
        case RCVCMD_NOTICE_SERVER_STOPPED:
            break;
    }
    if (code != RCVCMD_NOTICE_SERVER_STOPPED) {
        trace(NULL, 1, "%s\n", line);
    }
    io->notice(io->arg, &value);
}
/* read receiver commands ----------------------------------------------------*/
static int readcmd(const char* file, char* cmd, int type) {
    FILE* fp;
    char buff[MAXSTR], *p = cmd;
    int i = 0;

    trace(NULL, 3, "readcmd: file=%s\n", file);

    if (!(fp = fopen(file, "r"))) {
        return 0;
    }

    while (fgets(buff, sizeof(buff), fp)) {
        if (*buff == '@') {
            i++;
        } else if (i == type && p + strlen(buff) + 1 < cmd + MAXRCVCMD) {
            p += sprintf(p, "%s", buff);
        }
    }
    fclose(fp);
    return 1;
}
/* read antenna file ---------------------------------------------------------*/
static void readant(rcvcmd_t* cmd, const rcvcmd_io_t* io, prcopt_t* opt, nav_t* nav) {
    rcvcmd_settings_t* settings = &cmd->run_settings;
    const pcv_t pcv0 = {0};
    pcvs_t pcvr = {0}, pcvs = {0};
    pcv_t* pcv;
    gtime_t time = timeget();
    int i;

    trace(NULL, 3, "readant:\n");

    opt->pcvr[0] = opt->pcvr[1] = pcv0;
    if (!*settings->filopt.rcvantp) {
        return;
    }

    if (readpcv(settings->filopt.rcvantp, &pcvr)) {
        for (i = 0; i < 2; i++) {
            if (!*opt->anttype[i]) {
                continue;
            }
            if (!(pcv = searchpcv(0, opt->anttype[i], time, &pcvr))) {
                notice(io, RCVCMD_NOTICE_NO_ANTENNA, settings->filopt.rcvantp, opt->anttype[i], 0);
                continue;
            }
            opt->pcvr[i] = *pcv;
        }
    } else {
        notice(io, RCVCMD_NOTICE_ANTENNA_FILE_ERROR, settings->filopt.rcvantp, NULL, 0);
    }

    if (readpcv(settings->filopt.satantp, &pcvs)) {
        for (i = 0; i < MAXSAT; i++) {
            if (!(pcv = searchpcv(i + 1, "", time, &pcvs))) {
                continue;
            }
            nav->pcvs[i] = *pcv;
        }
    } else {
        notice(io, RCVCMD_NOTICE_ANTENNA_FILE_ERROR, settings->filopt.satantp, NULL, 0);
    }

    free(pcvr.pcv);
    free(pcvs.pcv);
}
/* Match stream/mrtk_stream.c openfile() option parsing and openfile_() reppath.
 * Time-tag inputs reset the process clock before outputs open, so their output
 * keyword names cannot be predicted here. Keep the unresolved name in that case. */
static void overwrite_path(const rcvcmd_settings_t* settings, const char* configured, char* out) {
    char path[1024];
    const char* option;
    int i, replay = 0;
    for (i = 0; i < 3; i++) {
        if (settings->strtype[i] != STR_FILE) continue;
        for (option = settings->strpath[i]; (option = strstr(option, "::")); option += 2)
            if (option[2] == 'T') replay = 1;
    }
    strcpy(path, configured);
    if (strstr(path, "::")) *strstr(path, "::") = '\0';
    if (replay)
        strcpy(out, path);
    else
        reppath(path, out, utc2gpst(timeget()), "", "");
}

/* start rtk server ----------------------------------------------------------*/
static rcvcmd_start_error_t start_server(rcvcmd_t* cmd, const rcvcmd_io_t* io, int stdin_reserved, char* refused) {
    rtksvr_t* svr = cmd->svr;
    rcvcmd_settings_t* settings = &cmd->run_settings;
    static sta_t sta[MAXRCV] = {{""}};
    double pos[3], npos[3];
    char s1[3][MAXRCVCMD] = {"", "", ""}, *cmds[] = {NULL, NULL, NULL};
    char s2[3][MAXRCVCMD] = {"", "", ""}, *cmds_periodic[] = {NULL, NULL, NULL};
    char* ropts[] = {"", "", ""};
    char* paths[8];
    char errmsg[2048] = "";
    int i, ret, stropt[8] = {0};

    for (i = 0; i < 8; i++) {
        paths[i] = settings->strpath[i];
    }
    trace(NULL, 3, "startsvr:\n");

    /* openfile() strips :: options before choosing stdin for an empty file path.
     * Refuse before commands or streams start so the watcher is the sole reader. */
    for (i = 0; stdin_reserved && i < 3; i++) {
        if (settings->strtype[i] == STR_FILE &&
            (!settings->strpath[i][0] || strncmp(settings->strpath[i], "::", 2) == 0)) {
            notice(io, RCVCMD_NOTICE_SERVER_START_ERROR, NULL, "stdin is reserved by --exit-on-stdin-eof", 0);
            return RCVCMD_START_ERROR_STDIN_RESERVED;
        }
    }

    /* resolve/validate correction source against the positioning mode */
    {
        char cmsg[256] = "";
        if (!resolve_correction(&settings->prcopt, cmsg, sizeof(cmsg))) {
            notice(io, RCVCMD_NOTICE_CORRECTION_ERROR, NULL, cmsg, 0);
            return RCVCMD_START_ERROR_CORRECTION;
        }
    }

    /* #184: apply PPP signal selection in the real-time path, mirroring the
     * post-processing path (rnx2rtkp.c). apply_pppsig() reshapes the obsdef
     * tables (GPS/QZS/GAL/BDS) that every receiver decoder consults via
     * code2freq_idx(); without it a non-GPS 2nd band that differs from the
     * default slot (e.g. GAL E5b, BDS B2I) is never mapped, so the madocalib
     * PPP engine can only form the iono-free pair for GPS. As in post, IGS
     * precise-product PPP skips it (#135) so the receiver's actual 2nd band
     * survives. (GLONASS is unaffected: apply_pppsig() does not touch it and
     * its default G1/G2 obsdef already maps correctly.)
     *
     * #186: reset to the pristine defaults first so a restart (the rtkrcv shell
     * can `load` a new config and `restart` in the same process) that changes
     * the correction or signal selection — including switching to correction=igs,
     * which skips apply_pppsig — cannot inherit a previously trimmed obsdef.
     * sigcfg-configured obsdef (pos1-signals) is left untouched. */
    if (!settings->prcopt.sigcfg_set) {
        reset_obsdef();
        if (settings->prcopt.correction != CORR_IGS) {
            apply_pppsig(settings->prcopt.pppsig);
        }
    }

    /* read start commads from command files */
    for (i = 0; i < 3; i++) {
        if (!*settings->rcvcmds[i]) {
            continue;
        }
        if (!readcmd(settings->rcvcmds[i], s1[i], 0)) {
            notice(io, RCVCMD_NOTICE_NO_COMMAND_FILE, settings->rcvcmds[i], NULL, 0);
        } else {
            cmds[i] = s1[i];
        }
        if (!readcmd(settings->rcvcmds[i], s2[i], 2)) {
            notice(io, RCVCMD_NOTICE_NO_COMMAND_FILE, settings->rcvcmds[i], NULL, 0);
        } else {
            cmds_periodic[i] = s2[i];
        }
    }
    /* Retain slot order, including when an adapter asks to continue after a refusal. */
    {
        int denied = 0;
        size_t used = 0;
        for (i = 3; i < 8; i++) {
            char path[1024];
            int answer;
            size_t offset, length;
            if (settings->strtype[i] != STR_FILE) continue;
            overwrite_path(settings, settings->strpath[i], path);
            answer = io->confirm_overwrite(io->arg, path);
            if (answer == RCVCMD_OVERWRITE_ALLOW) continue;
            denied = 1;
            for (offset = 0; offset < used; offset += strlen(refused + offset) + 1)
                if (!strcmp(refused + offset, path)) break;
            length = strlen(path) + 1;
            if (offset == used && length < RCVEVENT_PATHS - used) {
                memcpy(refused + used, path, length);
                used += length;
            }
            if (answer == RCVCMD_OVERWRITE_REFUSE_STOP) break;
        }
        if (denied) return RCVCMD_START_ERROR_OVERWRITE;
    }
    if (settings->prcopt.refpos == 4) { /* rtcm */
        for (i = 0; i < 3; i++) {
            settings->prcopt.rb[i] = 0.0;
        }
    }
    pos[0] = settings->nmeapos[0] * D2R;
    pos[1] = settings->nmeapos[1] * D2R;
    pos[2] = settings->nmeapos[2];
    pos2ecef(pos, npos);

    /* read antenna file */
    readant(cmd, io, &settings->prcopt, &svr->nav);

    /* read dcb file */
    if (*settings->filopt.dcb) {
        strcpy(sta[0].name, settings->sta_name);
        readdcb(settings->filopt.dcb, &svr->nav, sta);
    }
    /* open geoid data file */
    if (settings->solopt[0].geoid > 0 && !opengeoid(settings->solopt[0].geoid, settings->filopt.geoid)) {
        notice(io, RCVCMD_NOTICE_GEOID_FILE_ERROR, settings->filopt.geoid, NULL, 0);
    }
    /* set stream options */
    stropt[0] = settings->timeout;
    stropt[1] = settings->reconnect;
    stropt[2] = 1000;
    stropt[3] = settings->buffsize;
    stropt[4] = settings->fswapmargin;
    strsetopt(stropt);

    if (settings->strfmt[2] == 8) {
        settings->strfmt[2] = STRFMT_SP3;
    }

    /* set ftp/http directory and proxy */
    strsetdir(settings->filopt.tempdir);
    strsetproxy(settings->proxyaddr);

    /* execute start command */
    if (*settings->startcmd && (ret = execcmd(settings->startcmd))) {
        notice(io, RCVCMD_NOTICE_START_COMMAND_ERROR, NULL, settings->startcmd, ret);
    }
    settings->solopt[0].posf = settings->strfmt[3];
    settings->solopt[1].posf = settings->strfmt[4];

    /* start rtk server */
    if (!rtksvrstart(svr, settings->svrcycle, settings->buffsize, settings->strtype, paths, settings->strfmt,
                     settings->navmsgsel, cmds, cmds_periodic, ropts, settings->nmeacycle, settings->nmeareq, npos,
                     &settings->prcopt, settings->solopt, &cmd->moni, settings->rst, errmsg)) {
        char safe[2048];
        mask_stream_error(errmsg, safe, sizeof(safe));
        fprintf(stderr, "rtk server start error: %s\n", safe);
        if (settings->solopt[0].geoid > 0) {
            closegeoid();
        }
        notice(io, RCVCMD_NOTICE_SERVER_START_ERROR, NULL, errmsg, 0);
        return RCVCMD_START_ERROR_SERVER;
    }
    /* load CLAS auxiliary files after server start.
     * lock to avoid racing with the rtksvr thread */
    if (svr->clas) {
        rtksvrlock(svr);
        /* grid definition file */
        if (*settings->filopt.grid) {
            if (clas_read_grid_def(svr->clas, settings->filopt.grid) != 0) {
                fprintf(stderr, "clas grid file error: %s\n", settings->filopt.grid);
            }
        }
        /* ISB correction table */
        if (settings->prcopt.isb == ISBOPT_TABLE && *settings->filopt.isb) {
            readisb(settings->filopt.isb, &svr->nav);
        }
        /* L2C phase shift table */
        if (settings->prcopt.phasshft == ISBOPT_TABLE && *settings->filopt.phacyc) {
            readL2C(settings->filopt.phacyc, &svr->nav);
        }
        /* grid BLQ for OTL (tidecorr=3) */
        if (settings->prcopt.tidecorr == 3 && *settings->filopt.blq) {
            readblqgrid(settings->filopt.blq, svr->clas);
        }
        /* receiver OTL */
        if (settings->prcopt.mode > PMODE_SINGLE && *settings->filopt.blq) {
            readblq(settings->filopt.blq, sta[0].name, settings->prcopt.odisp[0]);
        }
        /* station info and ISB setup */
        memcpy(svr->nav.stas, sta, sizeof(sta_t) * MAXRCV);
        if (settings->prcopt.isb == ISBOPT_TABLE) {
            setisb(&svr->nav, settings->prcopt.rectype[0], settings->prcopt.rectype[1], &svr->nav.stas[0],
                   &svr->nav.stas[1]);
        }
        rtksvrunlock(svr);
    }
    return RCVCMD_START_ERROR_NONE;
}
/* stop rtk server -----------------------------------------------------------*/
static void stop_server(rcvcmd_t* cmd, const rcvcmd_io_t* io) {
    rtksvr_t* svr = cmd->svr;
    rcvcmd_settings_t* settings = &cmd->run_settings;
    char s[3][MAXRCVCMD] = {"", "", ""}, *cmds[] = {NULL, NULL, NULL};
    int i, ret;

    trace(NULL, 3, "stopsvr:\n");

    /* read stop commads from command files */
    for (i = 0; i < 3; i++) {
        if (!*settings->rcvcmds[i]) {
            continue;
        }
        if (!readcmd(settings->rcvcmds[i], s[i], 1)) {
            notice(io, RCVCMD_NOTICE_NO_COMMAND_FILE, settings->rcvcmds[i], NULL, 0);
        } else {
            cmds[i] = s[i];
        }
    }
    /* stop rtk server */
    rtksvrstop(svr, cmds);
    collect_events(cmd);
    pthread_mutex_lock(&cmd->publish_lock);
    publish_collected(cmd);
    pthread_mutex_unlock(&cmd->publish_lock);

    /* execute stop command */
    if (*settings->stopcmd && (ret = execcmd(settings->stopcmd))) {
        notice(io, RCVCMD_NOTICE_STOP_COMMAND_ERROR, NULL, settings->stopcmd, ret);
    }
    if (settings->solopt[0].geoid > 0) {
        closegeoid();
    }

    notice(io, RCVCMD_NOTICE_SERVER_STOPPED, NULL, NULL, 0);
}

void rcvcmd_get_lifecycle(rcvcmd_t* cmd, rcvcmd_lifecycle_t* data) {
    pthread_mutex_lock(&cmd->publish_lock);
    *data = cmd->lifecycle;
    pthread_mutex_unlock(&cmd->publish_lock);
}

rcvcmd_result_t rcvcmd_claim(rcvcmd_t* cmd, rcvcmd_operation_t operation, rcvcmd_claim_t* claim) {
    rcvcmd_result_t result = {RCVCMD_SUCCESS, RCVCMD_STOPPED, 0};
    int stop, start = operation == RCVCMD_OP_START || operation == RCVCMD_OP_RESTART;
    claim->seq = 0;
    pthread_mutex_lock(&cmd->publish_lock);
    if (operation == RCVCMD_OP_SHUTDOWN)
        while (cmd->claimed) pthread_cond_wait(&cmd->publish_cond, &cmd->publish_lock);
    result.seq = cmd->events.last;
    if (cmd->shutdown_requested && operation != RCVCMD_OP_SHUTDOWN)
        result.kind = RCVCMD_SHUTTING_DOWN;
    else if (cmd->claimed)
        result.kind = RCVCMD_BUSY;
    else if (operation == RCVCMD_OP_START && cmd->lifecycle.state == RCVCMD_RUNNING)
        result.kind = RCVCMD_ALREADY_RUNNING;
    else if (start || cmd->lifecycle.state == RCVCMD_RUNNING) {
        stop = cmd->lifecycle.state == RCVCMD_RUNNING && operation != RCVCMD_OP_START;
        cmd->claimed = 1;
        cmd->executing = 0;
        cmd->operation = operation;
        cmd->lifecycle.state = stop ? RCVCMD_STOPPING : RCVCMD_STARTING;
        if (stop) cmd->lifecycle.stop_reason = RCVCMD_STOP_REQUEST;
        claim->seq = cmd->claim_seq = result.seq = publish_status(cmd);
        if (!stop) cmd->solution_start_seq = result.seq;
        while (cmd->readers) pthread_cond_wait(&cmd->publish_cond, &cmd->publish_lock);
        if (!stop) clear_run_solution(cmd);
    }
    result.state = cmd->lifecycle.state;
    pthread_mutex_unlock(&cmd->publish_lock);
    return result;
}

rcvcmd_result_t rcvcmd_execute(rcvcmd_t* cmd, const rcvcmd_claim_t* claim, const rcvcmd_io_t* io) {
    rcvcmd_result_t result = {RCVCMD_SUCCESS, RCVCMD_STOPPED, claim->seq};
    rcvcmd_start_error_t error = RCVCMD_START_ERROR_NONE;
    rcvcmd_operation_t operation;
    char paths[RCVEVENT_PATHS] = {0};
    int stop, start, stdin_reserved;
    pthread_mutex_lock(&cmd->publish_lock);
    if (!claim->seq || !cmd->claimed || cmd->executing || cmd->claim_seq != claim->seq) {
        result.kind = RCVCMD_BUSY;
        result.state = cmd->lifecycle.state;
        pthread_mutex_unlock(&cmd->publish_lock);
        return result;
    }
    cmd->executing = 1;
    operation = cmd->operation;
    stop = cmd->lifecycle.state == RCVCMD_STOPPING;
    start = operation == RCVCMD_OP_START || operation == RCVCMD_OP_RESTART;
    pthread_mutex_unlock(&cmd->publish_lock);
    if (stop) {
        stop_server(cmd, io);
        if (operation == RCVCMD_OP_RESTART) {
            pthread_mutex_lock(&cmd->publish_lock);
            cmd->lifecycle.state = RCVCMD_STOPPED;
            publish_status(cmd);
            cmd->lifecycle.state = RCVCMD_STARTING;
            clear_run_solution(cmd);
            cmd->solution_start_seq = publish_status(cmd);
            pthread_mutex_unlock(&cmd->publish_lock);
        }
    }
    if (start) {
        pthread_mutex_lock(&cmd->config_lock);
        cmd->run_settings = cmd->settings;
        stdin_reserved = cmd->stdin_reserved;
        pthread_mutex_unlock(&cmd->config_lock);
        error = start_server(cmd, io, stdin_reserved, paths);
    }
    if (start && error == RCVCMD_START_ERROR_NONE) {
        /* Marks mean "not in effect yet". Changes are refused while starting
         * (only console-* is accepted, never marked), so these are the marks
         * of the copy that is now running. */
        pthread_mutex_lock(&cmd->config_lock);
        rcvcmd_clear_modified(cmd);
        pthread_mutex_unlock(&cmd->config_lock);
    }

    pthread_mutex_lock(&cmd->publish_lock);
    if (!start) {
        cmd->lifecycle.state = RCVCMD_STOPPED;
    } else if (error == RCVCMD_START_ERROR_NONE) {
        cmd->lifecycle.state = RCVCMD_RUNNING;
        cmd->run_mode = cmd->run_settings.prcopt.mode;
        cmd->run_geoid = cmd->run_settings.solopt[0].geoid;
        cmd->lifecycle.last_error = RCVCMD_START_ERROR_NONE;
        memset(cmd->lifecycle.paths, 0, sizeof(cmd->lifecycle.paths));
    } else {
        cmd->lifecycle.state = RCVCMD_STOPPED;
        cmd->lifecycle.stop_reason = RCVCMD_STOP_ERROR;
        cmd->lifecycle.last_error = error;
        memcpy(cmd->lifecycle.paths, paths, sizeof(paths));
        result.kind = RCVCMD_START_FAILED;
    }
    publish_status(cmd);
    cmd->claimed = 0;
    pthread_cond_broadcast(&cmd->publish_cond);
    result.state = cmd->lifecycle.state;
    pthread_mutex_unlock(&cmd->publish_lock);
    return result;
}

static rcvcmd_result_t run_lifecycle(rcvcmd_t* cmd, const rcvcmd_io_t* io, rcvcmd_operation_t operation) {
    rcvcmd_claim_t claim;
    rcvcmd_result_t result = rcvcmd_claim(cmd, operation, &claim);
    return claim.seq ? rcvcmd_execute(cmd, &claim, io) : result;
}

rcvcmd_result_t rcvcmd_start(rcvcmd_t* cmd, const rcvcmd_io_t* io) { return run_lifecycle(cmd, io, RCVCMD_OP_START); }

void rcvcmd_reserve_stdin(rcvcmd_t* cmd) {
    pthread_mutex_lock(&cmd->config_lock);
    cmd->stdin_reserved = 1;
    pthread_mutex_unlock(&cmd->config_lock);
}

rcvcmd_result_t rcvcmd_stop(rcvcmd_t* cmd, const rcvcmd_io_t* io) { return run_lifecycle(cmd, io, RCVCMD_OP_STOP); }

rcvcmd_result_t rcvcmd_restart(rcvcmd_t* cmd, const rcvcmd_io_t* io) {
    return run_lifecycle(cmd, io, RCVCMD_OP_RESTART);
}

void rcvcmd_request_shutdown(rcvcmd_t* cmd) {
    pthread_mutex_lock(&cmd->publish_lock);
    cmd->shutdown_requested = 1;
    pthread_mutex_unlock(&cmd->publish_lock);
}

int rcvcmd_shutdown_requested(rcvcmd_t* cmd) {
    int requested;
    pthread_mutex_lock(&cmd->publish_lock);
    requested = cmd->shutdown_requested;
    pthread_mutex_unlock(&cmd->publish_lock);
    return requested;
}

void rcvcmd_shutdown(rcvcmd_t* cmd, const rcvcmd_io_t* io) { run_lifecycle(cmd, io, RCVCMD_OP_SHUTDOWN); }

static int begin_read(rcvcmd_t* cmd) {
    int accepted;
    pthread_mutex_lock(&cmd->publish_lock);
    accepted = !cmd->claimed && (cmd->lifecycle.state == RCVCMD_RUNNING || cmd->lifecycle.state == RCVCMD_STOPPED);
    if (accepted) {
        cmd->readers++;
    }
    pthread_mutex_unlock(&cmd->publish_lock);
    return accepted;
}

static void end_read(rcvcmd_t* cmd) {
    pthread_mutex_lock(&cmd->publish_lock);
    if (--cmd->readers == 0) {
        pthread_cond_broadcast(&cmd->publish_cond);
    }
    pthread_mutex_unlock(&cmd->publish_lock);
}

/* display readers -----------------------------------------------------------*/
int rcvcmd_read_status(rcvcmd_t* cmd, rcvcmd_status_t* data) {
    rtksvr_t* svr = cmd->svr;
    int i, j;

    if (!begin_read(cmd)) {
        memset(data, 0, sizeof(*data));
        return 0;
    }
    rtksvrlock(svr);
    data->runtime = 0.0;
    data->has_x = data->has_P = data->has_xa = data->has_Pa = 0;
    memset(data->x3, 0, sizeof(data->x3));
    memset(data->P_diag, 0, sizeof(data->P_diag));
    memset(data->xa3, 0, sizeof(data->xa3));
    memset(data->Pa_diag, 0, sizeof(data->Pa_diag));
    data->rtk = svr->rtk;
    data->thread = (int)svr->thread;
    data->cycle = svr->cycle;
    data->state = svr->state;
    data->rtkstat = svr->rtk.sol.stat;
    data->nsat0 = svr->obs[0][0].n;
    data->nsat1 = svr->obs[1][0].n;
    data->cputime = svr->cputime;
    data->prcout = svr->prcout;
    data->nave = svr->nave;
    for (i = 0; i < 3; i++) {
        data->nb[i] = svr->nb[i];
    }
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 10; j++) {
            data->nmsg[i][j] = svr->nmsg[i][j];
        }
    }
    if (svr->state) {
        data->runtime = (double)(tickget() - svr->tick) / 1000.0;
    }
    for (i = 0; i < 3; i++) {
        memcpy(data->nmsg2[i], svr->rtcm[i].nmsg2, sizeof(data->nmsg2[i]));
        memcpy(data->nmsg3[i], svr->rtcm[i].nmsg3, sizeof(data->nmsg3[i]));
    }
    /* deep copy state vector values to avoid data race after unlock (#74) */
    if (svr->rtk.x) {
        data->has_x = 1;
        memcpy(data->x3, svr->rtk.x, sizeof(double) * 3);
    }
    if (svr->rtk.P) {
        data->has_P = 1;
        data->P_diag[0] = svr->rtk.P[0];
        data->P_diag[1] = svr->rtk.P[1 + 1 * svr->rtk.nx];
        data->P_diag[2] = svr->rtk.P[2 + 2 * svr->rtk.nx];
    }
    if (svr->rtk.xa) {
        data->has_xa = 1;
        memcpy(data->xa3, svr->rtk.xa, sizeof(double) * 3);
    }
    if (svr->rtk.Pa) {
        data->has_Pa = 1;
        data->Pa_diag[0] = svr->rtk.Pa[0];
        data->Pa_diag[1] = svr->rtk.Pa[1 + 1 * svr->rtk.na];
        data->Pa_diag[2] = svr->rtk.Pa[2 + 2 * svr->rtk.na];
    }
    /* null out shared pointers to prevent accidental access after unlock */
    data->rtk.x = data->rtk.P = data->rtk.Q = data->rtk.xa = data->rtk.Pa = NULL;
    rtksvrunlock(svr);
    end_read(cmd);
    return 1;
}

int rcvcmd_read_satellite(rcvcmd_t* cmd, rcvcmd_satellite_t* data) {
    rtksvr_t* svr = cmd->svr;

    if (!begin_read(cmd)) {
        memset(data, 0, sizeof(*data));
        return 0;
    }
    rtksvrlock(svr);
    data->rtk = svr->rtk;
    rtksvrunlock(svr);
    end_read(cmd);
    return 1;
}

int rcvcmd_read_observ(rcvcmd_t* cmd, int sys, rcvcmd_observ_t* data) {
    rtksvr_t* svr = cmd->svr;
    int i, n = 0;

    if (!begin_read(cmd)) {
        memset(data, 0, sizeof(*data));
        return 0;
    }
    rtksvrlock(svr);
    for (i = 0; i < svr->obs[0][0].n && n < MAXOBS * 2; i++) {
        if (!(satsys(svr->obs[0][0].data[i].sat, NULL) & sys)) {
            continue;
        }
        data->obs[n++] = svr->obs[0][0].data[i];
    }
    for (i = 0; i < svr->obs[1][0].n && n < MAXOBS * 2; i++) {
        if (!(satsys(svr->obs[1][0].data[i].sat, NULL) & sys)) {
            continue;
        }
        data->obs[n++] = svr->obs[1][0].data[i];
    }
    data->n = n;
    rtksvrunlock(svr);
    end_read(cmd);
    return 1;
}

int rcvcmd_read_navidata(rcvcmd_t* cmd, rcvcmd_navidata_t* data) {
    rtksvr_t* svr = cmd->svr;
    int i;

    if (!begin_read(cmd)) {
        memset(data, 0, sizeof(*data));
        return 0;
    }
    rtksvrlock(svr);
    data->time = svr->rtk.sol.time;
    for (i = 0; i < MAXSAT * 2; i++) {
        data->eph[i] = svr->nav.eph[i];
    }
    for (i = 0; i < MAXPRNGLO; i++) {
        data->geph[i] = svr->nav.geph[i];
    }
    for (i = 0; i < 8; i++) {
        data->ion[i] = svr->nav.ion_gps[i];
    }
    for (i = 0; i < 8; i++) {
        data->utc[i] = svr->nav.utc_gps[i];
    }
    rtksvrunlock(svr);
    end_read(cmd);
    return 1;
}

int rcvcmd_read_ssr(rcvcmd_t* cmd, rcvcmd_ssr_t* data) {
    rtksvr_t* svr = cmd->svr;
    int i;

    if (!begin_read(cmd)) {
        memset(data, 0, sizeof(*data));
        return 0;
    }
    rtksvrlock(svr);
    data->time = svr->rtk.sol.time;
    for (i = 0; i < MAXSAT; i++) {
        data->ssr[i] = svr->nav.ssr_ch[0][i];
    }
    rtksvrunlock(svr);
    end_read(cmd);
    return 1;
}

/* Copy only display fields: even while held, a mutex can be changed by
 * waiting threads, so copying the whole stream would still read its lock. */
static void copy_stream(stream_t* stream, stream_t* data) {
    strlock(stream);
    data->type = stream->type;
    data->state = stream->state;
    data->inb = stream->inb;
    data->inr = stream->inr;
    data->outb = stream->outb;
    data->outr = stream->outr;
    memcpy(data->path, stream->path, sizeof(data->path));
    memcpy(data->msg, stream->msg, sizeof(data->msg));
    strunlock(stream);
}

int rcvcmd_read_stream(rcvcmd_t* cmd, rcvcmd_stream_t* data) {
    rtksvr_t* svr = cmd->svr;
    int i;

    if (!begin_read(cmd)) {
        memset(data, 0, sizeof(*data));
        return 0;
    }
    rtksvrlock(svr);
    memset(data->format, 0, sizeof(data->format));
    for (i = 0; i < 3; i++) {
        data->format[i] = svr->format[i];
    }
    for (i = 3; i < 5; i++) {
        data->format[i] = svr->solopt[i - 3].posf;
    }
    data->format[8] = SOLF_LLH;
    rtksvrunlock(svr);

    for (i = 0; i < 8; i++) {
        copy_stream(&svr->stream[i], &data->stream[i]);
    }
    copy_stream(&cmd->moni, &data->stream[8]);
    end_read(cmd);
    return 1;
}

static int read_errors_locked(rcvcmd_t* cmd, uint64_t* cursor, rcvcmd_error_t* data) {
    uint64_t seq, first = rcvevent_oldest(&cmd->events);
    data->n = 0;
    if (*cursor >= cmd->events.last) {
        data->errbuf[0] = '\0';
        return 0;
    }
    if (first < *cursor + 1) first = *cursor + 1;
    for (seq = first; seq <= cmd->events.last; seq++) {
        const rcvevent_t* ev = rcvevent_get(&cmd->events, seq);
        if (ev->kind == RCVEVENT_LOG && (cmd->log_level == 2 || ev->data.log.origin == RCVEVENT_LOG_ERRBUF)) {
            size_t n = strlen(ev->data.log.text);
            if (n + 1 > (size_t)(MAXERRMSG - data->n)) break;
            memcpy(data->errbuf + data->n, ev->data.log.text, n);
            data->n += (int)n;
            data->errbuf[data->n++] = '\n';
        }
        *cursor = seq;
    }
    data->errbuf[data->n] = '\0';
    return data->n;
}

uint64_t rcvcmd_last_seq(rcvcmd_t* cmd) {
    uint64_t seq;
    pthread_mutex_lock(&cmd->publish_lock);
    seq = cmd->events.last;
    pthread_mutex_unlock(&cmd->publish_lock);
    return seq;
}

void rcvcmd_snapshot(rcvcmd_t* cmd, rcvcmd_snapshot_t* data) {
    pthread_mutex_lock(&cmd->publish_lock);
    memset(data, 0, sizeof(*data));
    data->seq = cmd->events.last;
    data->lifecycle = cmd->lifecycle;
    if (cmd->lifecycle.state == RCVCMD_RUNNING) {
        data->mode = cmd->run_mode;
        data->has_solution = cmd->has_solution;
        if (cmd->has_solution) data->solution = cmd->last_solution;
    }
    pthread_mutex_unlock(&cmd->publish_lock);
}

int rcvcmd_read_events(rcvcmd_t* cmd, uint64_t after, rcvevent_t* out, int limit, rcvevent_read_t* res) {
    int result;
    pthread_mutex_lock(&cmd->publish_lock);
    result = rcvevent_read(&cmd->events, after, out, limit, res);
    pthread_mutex_unlock(&cmd->publish_lock);
    return result;
}

int rcvcmd_read_solutions(rcvcmd_t* cmd, uint64_t* cursor, rcvcmd_solution_t* data) {
    uint64_t after;
    pthread_mutex_lock(&cmd->publish_lock);
    after = *cursor > cmd->solution_start_seq ? *cursor : cmd->solution_start_seq;
    data->nsol = rcvevent_read_newest(&cmd->events, after, RCVEVENT_SOLUTION, data->events, MAXSOLBUF, cursor);
    pthread_mutex_unlock(&cmd->publish_lock);
    return data->nsol;
}

int rcvcmd_read_errors(rcvcmd_t* cmd, uint64_t* cursor, rcvcmd_error_t* data) {
    int n;
    pthread_mutex_lock(&cmd->publish_lock);
    n = read_errors_locked(cmd, cursor, data);
    pthread_mutex_unlock(&cmd->publish_lock);
    return n;
}
