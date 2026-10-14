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

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mrtklib/mrtk_antenna.h"
#include "mrtklib/mrtk_const.h"
#include "mrtklib/mrtk_coords.h"
#include "mrtklib/mrtk_geoid.h"
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
    rtksvr_t* svr;     /* rtk server struct */
    stream_t moni;     /* monitor stream */
    char passwd[1024]; /* login password */
    int timetype;      /* time format (0:gpst,1:utc,2:jst,3:tow) */
    int soltype;       /* sol format (0:dms,1:deg,2:xyz,3:enu,4:pyl) */
    int solflag;       /* sol flag (1:std+2:age/ratio/ns) */
    rcvcmd_settings_t settings;
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
rcvcmd_t* rcvcmd_create(void) {
    if (instance.svr) {
        return NULL;
    }
    /* heap-allocate rtksvr_t (~51 MB) to avoid BSS bloat */
    instance.svr = (rtksvr_t*)calloc(1, sizeof(rtksvr_t));
    if (!instance.svr) {
        fprintf(stderr, "error: rtksvr_t allocation failed\n");
        return NULL;
    }
    return &instance;
}

void rcvcmd_destroy(rcvcmd_t* cmd) {
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

const char* rcvcmd_password(rcvcmd_t* cmd) { return cmd->passwd; }
int rcvcmd_time_type(rcvcmd_t* cmd) { return cmd->timetype; }
int rcvcmd_solution_type(rcvcmd_t* cmd) { return cmd->soltype; }
int rcvcmd_solution_flags(rcvcmd_t* cmd) { return cmd->solflag; }
int rcvcmd_monitor_port(rcvcmd_t* cmd) { return cmd->moniport; }

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

int rcvcmd_set_option(rcvcmd_t* cmd, const rcvcmd_option_t* option, const char* value) {
    if (!str2opt(option->opt, value)) {
        return 0;
    }
    getsysopts(&cmd->settings.prcopt, cmd->settings.solopt, &cmd->settings.filopt);
    return 1;
}

int rcvcmd_mark_option_modified(rcvcmd_t* cmd, const rcvcmd_option_t* option) {
    int* modf = option->system ? cmd->modflgs + option->index : cmd->modflgr + option->index;
    if (strncmp(option->opt->name, "console", 7)) {
        *modf = 1;
        return 1;
    }
    return 0;
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

int rcvcmd_load(rcvcmd_t* cmd, const char* file) {
    resetsysopts();
    if (!loadopts(file, sysopts)) {
        return 0;
    }
    getsysopts(&cmd->settings.prcopt, cmd->settings.solopt, &cmd->settings.filopt);

    if (!loadopts(file, cmd->rcvopts)) {
        return 0;
    }
    return 1;
}

int rcvcmd_save(rcvcmd_t* cmd, const char* file) {
    char comment[256], s[64];
    time2str(utc2gpst(timeget()), s, 0);
    sprintf(comment, "%s options (%s, %s ver.%s)", PRGNAME, s, MRTKLIB_SOFTNAME, MRTKLIB_VERSION_STRING);
    setsysopts(&cmd->settings.prcopt, cmd->settings.solopt, &cmd->settings.filopt);
    if (!saveopts(file, "w", comment, cmd->rcvopts) || !saveopts(file, "a", NULL, sysopts)) {
        return 0;
    }
    return 1;
}

/* temporary until stage 3 */
rtksvr_t* rcvcmd_server(rcvcmd_t* cmd) { return cmd->svr; }
stream_t* rcvcmd_monitor(rcvcmd_t* cmd) { return &cmd->moni; }
/* temporary until stage 3 */
int rcvcmd_solution_height(rcvcmd_t* cmd) { return cmd->settings.solopt[0].height; }

/* adapter notice ------------------------------------------------------------*/
static void notice(const rcvcmd_io_t* io, rcvcmd_notice_code_t code, const char* path, const char* text, int status) {
    rcvcmd_notice_t value = {code, path, text, status};
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
    rcvcmd_settings_t* settings = &cmd->settings;
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
/* start rtk server ----------------------------------------------------------*/
int rcvcmd_start(rcvcmd_t* cmd, const rcvcmd_io_t* io) {
    rtksvr_t* svr = cmd->svr;
    rcvcmd_settings_t* settings = &cmd->settings;
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

    /* resolve/validate correction source against the positioning mode */
    {
        char cmsg[256] = "";
        if (!resolve_correction(&settings->prcopt, cmsg, sizeof(cmsg))) {
            trace(NULL, 1, "startsvr: %s\n", cmsg);
            notice(io, RCVCMD_NOTICE_CORRECTION_ERROR, NULL, cmsg, 0);
            return 0;
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
    /* confirm overwrite */
    for (i = 3; i < 8; i++) {
        if (settings->strtype[i] == STR_FILE && !io->confirm_overwrite(io->arg, settings->strpath[i])) {
            return 0;
        }
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
        trace(NULL, 2, "geoid data open error: %s\n", settings->filopt.geoid);
        notice(io, RCVCMD_NOTICE_GEOID_FILE_ERROR, settings->filopt.geoid, NULL, 0);
    }
    rcvcmd_clear_modified(cmd);

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
    if (*settings->startcmd && (ret = system(settings->startcmd))) {
        trace(NULL, 2, "command exec error: %s (%d)\n", settings->startcmd, ret);
        notice(io, RCVCMD_NOTICE_COMMAND_EXEC_ERROR, NULL, settings->startcmd, ret);
    }
    settings->solopt[0].posf = settings->strfmt[3];
    settings->solopt[1].posf = settings->strfmt[4];

    /* start rtk server */
    if (!rtksvrstart(svr, settings->svrcycle, settings->buffsize, settings->strtype, paths, settings->strfmt,
                     settings->navmsgsel, cmds, cmds_periodic, ropts, settings->nmeacycle, settings->nmeareq, npos,
                     &settings->prcopt, settings->solopt, &cmd->moni, settings->rst, errmsg)) {
        trace(NULL, 2, "rtk server start error (%s)\n", errmsg);
        fprintf(stderr, "rtk server start error: %s\n", errmsg);
        notice(io, RCVCMD_NOTICE_SERVER_START_ERROR, NULL, errmsg, 0);
        return 0;
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
    return 1;
}
/* stop rtk server -----------------------------------------------------------*/
void rcvcmd_stop(rcvcmd_t* cmd, const rcvcmd_io_t* io) {
    rtksvr_t* svr = cmd->svr;
    rcvcmd_settings_t* settings = &cmd->settings;
    char s[3][MAXRCVCMD] = {"", "", ""}, *cmds[] = {NULL, NULL, NULL};
    int i, ret;

    trace(NULL, 3, "stopsvr:\n");

    if (!svr->state) {
        return;
    }

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

    /* execute stop command */
    if (*settings->stopcmd && (ret = system(settings->stopcmd))) {
        trace(NULL, 2, "command exec error: %s (%d)\n", settings->stopcmd, ret);
        notice(io, RCVCMD_NOTICE_COMMAND_EXEC_ERROR, NULL, settings->stopcmd, ret);
    }
    if (settings->solopt[0].geoid > 0) {
        closegeoid();
    }

    notice(io, RCVCMD_NOTICE_SERVER_STOPPED, NULL, NULL, 0);
}
