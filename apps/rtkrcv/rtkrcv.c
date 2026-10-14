/*------------------------------------------------------------------------------
 * rtkrcv.c : rtk-gps/gnss receiver console ap
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
 * @file rtkrcv.c
 * @brief RTK-GNSS receiver real-time positioning console application.
 *
 * Notes:
 *   Current version does not support Win32 without pthread library.
 *
 * History:
 *   2009/12/13 1.0  new
 *   2010/07/18 1.1  add option -m
 *   2010/08/12 1.2  fix bug on ftp/http
 *   2011/01/22 1.3  add option misc-proxyaddr,misc-fswapmargin
 *   2011/08/19 1.4  fix bug on size of arg solopt arg for rtksvrstart()
 *   2012/11/03 1.5  fix bug on setting output format
 *   2013/06/30 1.6  add "nvs" option for inpstr*-format
 *   2014/02/10 1.7  fix bug on printing obs data
 *                   add print of status, glonass nav data
 *                   ignore SIGHUP
 *   2014/04/27 1.8  add "binex" option for inpstr*-format
 *   2014/08/10 1.9  fix cpu overload with abnormal telnet shutdown
 *   2014/08/26 1.10 support input format "rt17"
 *                   change file paths of solution status and debug trace
 *   2015/01/10 1.11 add line editting and command history
 *                   separate codes for virtual console to vt.c
 *   2015/05/22 1.12 fix bug on sp3 id in inpstr*-format options
 *   2015/07/31 1.13 accept 4:stat for outstr1-format or outstr2-format
 *                   add reading satellite dcb
 *   2015/12/14 1.14 add option -sta for station name (#339)
 *   2015/12/25 1.15 fix bug on -sta option (#339)
 *   2015/01/26 1.16 support septentrio
 *   2016/07/01 1.17 support CMR/CMR+
 *   2016/08/20 1.18 add output of patch level with version
 *   2016/09/05 1.19 support ntrip caster for output stream
 *   2016/09/19 1.20 support multiple remote console connections
 *                   add option -w
 *   2017/09/01 1.21 add command ssr
 *   2021/01/19 1.22 add option -v and -ver
 *              1.23 update option string ISTOPT,OSTOPT,FMTOPT
 *   2024/02/01 1.24 branch from ver.2.4.3b35 for MALIB
 *                   add option -rst
 *   2024/08/02 1.25 add stat format option
 *                   fix bug confwrite() con_close()
 *   2024/09/26 1.26 update version info
 *   2024/12/20 1.27 add option -sta
 *   2025/02/06 1.28 support Bias-SINEX and FCB files correction
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "mrtklib/mrtk_cli.h"
#include "mrtklib/mrtk_context.h"
#include "mrtklib/mrtklib.h"
#include "rcvcmd.h"
#include "rtklib.h"
#include "vt.h"

#define PRGNAME "rtkrcv" /* program name */
#ifndef PROGNAME
#define PROGNAME PRGNAME
#endif
#define CMDPROMPT "rtkrcv> "                /* command prompt */
#define MAXCON 32                           /* max number of consoles */
#define MAXARG 10                           /* max number of args in a command */
#define MAXCMD 256                          /* max length of a command */
#define MAXSTR 1024                         /* max length of a stream */
#define OPTSDIR "."                         /* default config directory */
#define OPTSFILE "rtkrcv.conf"              /* default config file */
#define TRACEFILE "rtkrcv_%Y%m%d%h%M.trace" /* debug trace file */

#define ESC_CLEAR "\033[H\033[2J" /* ansi/vt100 escape: erase screen */
#define ESC_RESET "\033[0m"       /* ansi/vt100: reset attribute */
#define ESC_BOLD "\033[1m"        /* ansi/vt100: bold */

#define SQRT(x) ((x) <= 0.0 || (x) != (x) ? 0.0 : sqrt(x))

/* type defintions -----------------------------------------------------------*/

typedef struct {      /* console type */
    int state;        /* state (0:stop,1:run) */
    vt_t* vt;         /* virtual terminal */
    pthread_t thread; /* console thread */
} con_t;

/* function prototypes -------------------------------------------------------*/
extern FILE* popen(const char*, const char*);
extern int pclose(FILE*);

/* global variables ----------------------------------------------------------*/
static rcvcmd_t* cmd; /* command layer instance */
static volatile sig_atomic_t intflg = 0;
static volatile sig_atomic_t shut_signal = 0;

/* help text -----------------------------------------------------------------*/
/* Long-option aliases for the command-line parser. Note rtkrcv historically
 * uses -o for the processing-options file (i.e. the configuration), -t for
 * debug trace, and -r for solution-status output level. */
static const mrtk_optmap_t opt_aliases[] = {
    {"--config", "-o"}, {"--device", "-d"}, {"--port", "-p"}, {"--trace", "-t"}, {NULL, NULL},
};

static const char* usage[] = {
    "mrtk run: real-time positioning console (rtkrcv)",
    "",
    "Usage: mrtk run [OPTIONS]",
    "",
    "  Starts the RTK server with optional telnet console. Stream paths,",
    "  formats, and processing options are loaded from a configuration file",
    "  (TOML or legacy). Without -s the server stays idle until the 'start'",
    "  console command is issued.",
    "",
    "Options:",
    "  -s                   Start RTK server on program startup",
    "  -p,  --port PORT     Telnet console port",
    "  -m   PORT            Monitor stream port",
    "  -d,  --device DEV    Terminal device for console",
    "  -o,  --config FILE   Processing options / configuration file",
    "  -w   PWD             Login password for remote console (\"\": none)",
    "  -r   LEVEL           Solution status file (0:off, 1:states, 2:residuals)",
    "  -t,  --trace LEVEL   Debug trace level (0:off, 1-5:on)",
    "  -sta NAME            Station name for receiver DCB",
    "  -v,  -ver            Print version",
    "  -rst Y/M/D H:M:S     Start day/time (raw/rtcm data start time)",
    "  -h,  --help          Show this help",
    "",
    "Examples:",
    "  mrtk run --config conf/run.toml --port 2401 -s",
    "  mrtk run -o conf.toml --device /dev/ttyUSB0 --trace 2",
    NULL,
};
static const char* helptxt[] = {"start                 : start rtk server",
                                "stop                  : stop rtk server",
                                "restart               : restart rtk sever",
                                "solution [cycle]      : show solution",
                                "status [cycle]        : show rtk status",
                                "satellite [-n] [cycle]: show satellite status",
                                "observ [-n] [cycle]   : show observation data",
                                "navidata [cycle]      : show navigation data",
                                "stream [cycle]        : show stream status",
                                "ssr [cycle]           : show ssr corrections",
                                "error                 : show error/warning messages",
                                "option [opt]          : show option(s)",
                                "set opt [val]         : set option",
                                "load [file]           : load options from file",
                                "save [file]           : save options to file",
                                "log [file|off]        : start/stop log to file",
                                "help|? [path]         : print help",
                                "exit|ctr-D            : logout console (only for telnet)",
                                "shutdown              : shutdown rtk server",
                                "!command [arg...]     : execute command in shell",
                                ""};
static const char* pathopts[] = {/* path options help */
                                 "stream path formats",
                                 "serial   : port[:bit_rate[:byte[:parity(n|o|e)[:stopb[:fctr(off|on)]]]]][#port*1]",
                                 "file     : path[::T[::+offset][::xspeed]]",
                                 "tcpsvr   : :port",
                                 "tcpcli   : addr:port",
                                 "ntripsvr : :passwd@addr:port/mntpnt[:str*2]",
                                 "ntripcli : [user[:passwd]]@addr:port/mntpnt",
                                 "ntripcas : [user[:passwd]]@:port/mntpnt[:str*2]",
                                 "ftp      : user:passwd@addr/path[::T=poff,tint,off,rint]",
                                 "http     : addr/path[::T=poff,tint,off,rint]",
                                 "udpsvr   : :port",
                                 "udpcli   : addr:port",
                                 "",
                                 " *1: tcp server port to output return data via the serial stream",
                                 " *2: string for source entry in ntrip sourcetable separated by :",
                                 ""};
/* print usage ---------------------------------------------------------------*/
static void printusage(void) {
    int i;
    for (i = 0; usage[i]; i++) {
        fprintf(stderr, "%s\n", usage[i]);
    }
    exit(0);
}
/* print version -------------------------------------------------------------*/
static void printver(void) {
    fprintf(stderr, "%s %s git %s\n", PRGNAME, MRTKLIB_VERSION_STRING, mrtklib_git_hash_str);
    exit(0);
}
/* external stop signal ------------------------------------------------------*/
static void sigshut(int sig) {
    shut_signal = sig;
    intflg = 1;
}
/* SIGSEGV handler (best-effort, async-signal-safe only) ---------------------*/
/* pre-built sigaction struct for restoring default handler (async-signal-safe:
 * avoids calling sigemptyset() inside the signal handler) */
static struct sigaction sa_default;

static void sigcrash(int sig) {
    /* Only async-signal-safe operations: write, sigaction, sigprocmask, kill,
     * _exit. Symbolization should be performed from a core dump if needed. */
    static const char msg[] = "\n*** SIGSEGV ***\n";
    sigset_t ss;

    (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);

    /* restore default handler */
    (void)sigaction(sig, &sa_default, NULL);

    /* unblock the signal so kill() delivers it immediately */
    sigemptyset(&ss);
    sigaddset(&ss, sig);
    (void)sigprocmask(SIG_UNBLOCK, &ss, NULL);

    /* re-send to produce a core dump under the default handler */
    (void)kill(getpid(), sig);

    /* fallback: should not reach here, but guard against it */
    _exit(128 + sig);
}
/* discard space characters at tail ------------------------------------------*/
static void chop(char* str) {
    char* p;
    for (p = str + strlen(str) - 1; p >= str && !isgraph((int)*p); p--) {
        *p = '\0';
    }
}
/* confirm overwrite ---------------------------------------------------------*/
static int confwrite(vt_t* vt, const char* file) {
    FILE* fp;
    char buff[MAXSTR], *p;

    /* Check if vt is NULL */
    if (vt == NULL) {
        fprintf(stderr, "Error: vt is NULL\n");
        return 1;
    }
    if (!vt->state) {
        fprintf(stderr, "Error: vt->state is invalid\n");
        return 1;
    }
    strcpy(buff, file);
    if ((p = strstr(buff, "::"))) {
        *p = '\0'; /* omit options in path */
    }
    if (!vt->state || !(fp = fopen(buff, "r"))) {
        return 1; /* no existing file */
    }
    fclose(fp);
    vt_printf(vt, "overwrite %-16s ? (y/n): ", buff);
    if (!vt_gets(vt, buff, sizeof(buff)) || vt->brk) {
        return 0;
    }
    return toupper((int)buff[0]) == 'Y';
}
/* lifecycle notices ---------------------------------------------------------*/
static void console_notice(void* arg, const rcvcmd_notice_t* notice) {
    vt_t* vt = (vt_t*)arg;
    switch (notice->code) {
        case RCVCMD_NOTICE_CORRECTION_ERROR:
            vt_printf(vt, "error: %s\n", notice->text);
            break;
        case RCVCMD_NOTICE_NO_COMMAND_FILE:
            vt_printf(vt, "no command file: %s\n", notice->path);
            break;
        case RCVCMD_NOTICE_NO_ANTENNA:
            vt_printf(vt, "no antenna %s in %s", notice->text, notice->path);
            break;
        case RCVCMD_NOTICE_ANTENNA_FILE_ERROR:
            vt_printf(vt, "antenna file open error %s", notice->path);
            break;
        case RCVCMD_NOTICE_GEOID_FILE_ERROR:
            vt_printf(vt, "geoid data open error: %s\n", notice->path);
            break;
        case RCVCMD_NOTICE_COMMAND_EXEC_ERROR:
            vt_printf(vt, "command exec error: %s (%d)\n", notice->text, notice->status);
            break;
        case RCVCMD_NOTICE_SERVER_START_ERROR:
            vt_printf(vt, "rtk server start error (%s)\n", notice->text);
            break;
        case RCVCMD_NOTICE_SERVER_STOPPED:
            vt_printf(vt, "stop rtk server\n");
            break;
    }
}

static int console_confirm_overwrite(void* arg, const char* path) { return confwrite((vt_t*)arg, path); }

/* Process start/shutdown used vt == NULL before the split: only correction
 * errors go to stderr here; other notices retain vt_printf(NULL, ...) behavior.
 * Overwrite confirmation still goes through confwrite(NULL, ...). */
static void process_notice(void* arg, const rcvcmd_notice_t* notice) {
    if (notice->code == RCVCMD_NOTICE_CORRECTION_ERROR) {
        fprintf(stderr, "error: %s\n", notice->text);
    } else {
        console_notice(arg, notice);
    }
}
static const rcvcmd_io_t process_io = {NULL, process_notice, console_confirm_overwrite};

/* login ---------------------------------------------------------------------*/
static int login(vt_t* vt) {
    char buff[256], password[1024];

    rcvcmd_password(cmd, password, sizeof(password));
    trace(NULL, 3, "login: passwd=%s type=%d\n", password, vt->type);

    if (!*password || !vt->type) {
        return 1;
    }

    while (1) {
        if (!vt_printf(vt, "password: ", PROGNAME)) {
            return 0;
        }
        vt->blind = 1;
        if (!vt_gets(vt, buff, sizeof(buff)) || vt->brk) {
            vt->blind = 0;
            return 0;
        }
        vt->blind = 0;
        rcvcmd_password(cmd, password, sizeof(password));
        if (!strcmp(buff, password)) {
            break;
        }
        vt_printf(vt, "\ninvalid password\n");
    }
    return 1;
}
/* print time ----------------------------------------------------------------*/
static void prtime(vt_t* vt, gtime_t time) {
    double tow;
    int week;
    char tstr[64] = "";

    if (rcvcmd_time_type(cmd) == 1) {
        time2str(gpst2utc(time), tstr, 2);
    } else if (rcvcmd_time_type(cmd) == 2) {
        time2str(timeadd(gpst2utc(time), 9 * 3600.0), tstr, 2);
    } else if (rcvcmd_time_type(cmd) == 3) {
        tow = time2gpst(time, &week);
        sprintf(tstr, "  %04d %9.2f", week, tow);
    } else {
        time2str(time, tstr, 1);
    }
    vt_printf(vt, "%s ", tstr);
}
/* print solution ------------------------------------------------------------*/
static void prsolution(vt_t* vt, const sol_t* sol, const double* rb) {
    const char* solstr[] = {"------", "FIX", "FLOAT", "SBAS", "DGPS", "SINGLE", "PPP", ""};
    double pos[3] = {0}, Qr[9], Qe[9] = {0}, dms1[3] = {0}, dms2[3] = {0}, bl[3] = {0};
    double enu[3] = {0}, pitch = 0.0, yaw = 0.0, len;
    int i;

    trace(NULL, 4, "prsolution:\n");

    if (sol->time.time == 0 || !sol->stat) {
        return;
    }
    prtime(vt, sol->time);
    vt_printf(vt, "(%-6s)", solstr[sol->stat]);

    if (norm(sol->rr, 3) > 0.0 && norm(rb, 3) > 0.0) {
        for (i = 0; i < 3; i++) {
            bl[i] = sol->rr[i] - rb[i];
        }
    }
    len = norm(bl, 3);
    Qr[0] = sol->qr[0];
    Qr[4] = sol->qr[1];
    Qr[8] = sol->qr[2];
    Qr[1] = Qr[3] = sol->qr[3];
    Qr[5] = Qr[7] = sol->qr[4];
    Qr[2] = Qr[6] = sol->qr[5];

    if (rcvcmd_solution_type(cmd) == 0) {
        if (norm(sol->rr, 3) > 0.0) {
            ecef2pos(sol->rr, pos);
            covenu(pos, Qr, Qe);
            deg2dms(pos[0] * R2D, dms1, 4);
            deg2dms(pos[1] * R2D, dms2, 4);
            if (rcvcmd_solution_height(cmd) == 1) {
                pos[2] -= geoidh(pos); /* geodetic */
            }
        }
        vt_printf(vt, " %s:%2.0f %02.0f %07.4f", pos[0] < 0 ? "S" : "N", fabs(dms1[0]), dms1[1], dms1[2]);
        vt_printf(vt, " %s:%3.0f %02.0f %07.4f", pos[1] < 0 ? "W" : "E", fabs(dms2[0]), dms2[1], dms2[2]);
        vt_printf(vt, " H:%8.3f", pos[2]);
        if (rcvcmd_solution_flags(cmd) & 1) {
            vt_printf(vt, " (N:%6.3f E:%6.3f U:%6.3f)", SQRT(Qe[4]), SQRT(Qe[0]), SQRT(Qe[8]));
        }
    } else if (rcvcmd_solution_type(cmd) == 1) {
        if (norm(sol->rr, 3) > 0.0) {
            ecef2pos(sol->rr, pos);
            covenu(pos, Qr, Qe);
            if (rcvcmd_solution_height(cmd) == 1) {
                pos[2] -= geoidh(pos); /* geodetic */
            }
        }
        vt_printf(vt, " %s:%11.8f", pos[0] < 0.0 ? "S" : "N", fabs(pos[0]) * R2D);
        vt_printf(vt, " %s:%12.8f", pos[1] < 0.0 ? "W" : "E", fabs(pos[1]) * R2D);
        vt_printf(vt, " H:%8.3f", pos[2]);
        if (rcvcmd_solution_flags(cmd) & 1) {
            vt_printf(vt, " (E:%6.3f N:%6.3f U:%6.3fm)", SQRT(Qe[0]), SQRT(Qe[4]), SQRT(Qe[8]));
        }
    } else if (rcvcmd_solution_type(cmd) == 2) {
        vt_printf(vt, " X:%12.3f", sol->rr[0]);
        vt_printf(vt, " Y:%12.3f", sol->rr[1]);
        vt_printf(vt, " Z:%12.3f", sol->rr[2]);
        if (rcvcmd_solution_flags(cmd) & 1) {
            vt_printf(vt, " (X:%6.3f Y:%6.3f Z:%6.3f)", SQRT(Qr[0]), SQRT(Qr[4]), SQRT(Qr[8]));
        }
    } else if (rcvcmd_solution_type(cmd) == 3) {
        if (len > 0.0) {
            ecef2pos(rb, pos);
            ecef2enu(pos, bl, enu);
            covenu(pos, Qr, Qe);
        }
        vt_printf(vt, " E:%12.3f", enu[0]);
        vt_printf(vt, " N:%12.3f", enu[1]);
        vt_printf(vt, " U:%12.3f", enu[2]);
        if (rcvcmd_solution_flags(cmd) & 1) {
            vt_printf(vt, " (E:%6.3f N:%6.3f U:%6.3f)", SQRT(Qe[0]), SQRT(Qe[4]), SQRT(Qe[8]));
        }
    } else if (rcvcmd_solution_type(cmd) == 4) {
        if (len > 0.0) {
            ecef2pos(rb, pos);
            ecef2enu(pos, bl, enu);
            covenu(pos, Qr, Qe);
            pitch = asin(enu[2] / len);
            yaw = atan2(enu[0], enu[1]);
            if (yaw < 0.0) {
                yaw += 2.0 * PI;
            }
        }
        vt_printf(vt, " P:%12.3f", pitch * R2D);
        vt_printf(vt, " Y:%12.3f", yaw * R2D);
        vt_printf(vt, " L:%12.3f", len);
        if (rcvcmd_solution_flags(cmd) & 1) {
            vt_printf(vt, " (E:%6.3f N:%6.3f U:%6.3f)", SQRT(Qe[0]), SQRT(Qe[4]), SQRT(Qe[8]));
        }
    }
    if (rcvcmd_solution_flags(cmd) & 2) {
        vt_printf(vt, " A:%4.1f R:%5.1f N:%2d", sol->age, sol->ratio, sol->ns);
    }
    vt_printf(vt, "\n");
}
/* print status --------------------------------------------------------------*/
static void prstatus(vt_t* vt) {
    rcvcmd_status_t* data;
    rtk_t* rtk;
    const char* svrstate[] = {"stop", "run"};
    const char* sol[] = {"-", "fix", "float", "SBAS", "DGPS", "single", "PPP", ""};
    const char* mode[] = {"single",     "DGPS",      "kinematic", "static",  "moving-base",   "fixed",  "PPP-kinema",
                          "PPP-static", "PPP-fixed", "PPP-RTK",   "SSR2OSR", "SSR2OSR-fixed", "VRS-RTK"};
    const char* freq[] = {"-", "L1", "L1+L2", "L1+L2+L3", "L1+L2+L3+L4", "L1+L2+L3+L4+L5", ""};
    uint32_t (*nmsg2)[100], (*nmsg3)[400];
    int i, j, n;
    int* nb;
    int (*nmsg)[10];
    char tstr[64], s[1024], *p, tmp[64];
    double runtime, rt[3] = {0}, dop[4] = {0}, rr[3], bl1 = 0.0, bl2 = 0.0;
    double azel[MAXSAT * 2], pos[3], vel[3], *del;
    double *x3, *P_diag, *xa3, *Pa_diag;

    trace(NULL, 4, "prstatus:\n");

    if (!(data = (rcvcmd_status_t*)malloc(sizeof(*data)))) {
        trace(NULL, 2, "prstatus: snapshot allocation failed\n");
        return;
    }
    rtk = &data->rtk;
    nmsg2 = data->nmsg2;
    nmsg3 = data->nmsg3;
    nb = data->nb;
    nmsg = data->nmsg;
    x3 = data->x3;
    P_diag = data->P_diag;
    xa3 = data->xa3;
    Pa_diag = data->Pa_diag;

    rcvcmd_read_status(cmd, data);
    if (data->state) {
        runtime = data->runtime;
        rt[0] = floor(runtime / 3600.0);
        runtime -= rt[0] * 3600.0;
        rt[1] = floor(runtime / 60.0);
        rt[2] = runtime - rt[1] * 60.0;
    }

    for (i = n = 0; i < MAXSAT; i++) {
        if (rtk->opt.mode == PMODE_SINGLE && !rtk->ssat[i].vs) {
            continue;
        }
        if (rtk->opt.mode != PMODE_SINGLE && !rtk->ssat[i].vsat[0]) {
            continue;
        }
        azel[n * 2] = rtk->ssat[i].azel[0];
        azel[1 + n * 2] = rtk->ssat[i].azel[1];
        n++;
    }
    dops(n, azel, 0.0, dop);

    vt_printf(vt, "\n%s%-28s: %s%s\n", ESC_BOLD, "Parameter", "Value", ESC_RESET);
    sprintf(tmp, "%s version", MRTKLIB_SOFTNAME);
    vt_printf(vt, "%-28s: %s (git %s)\n", tmp, MRTKLIB_VERSION_STRING, mrtklib_git_hash_str);
    vt_printf(vt, "%-28s: %d\n", "rtk server thread", data->thread);
    vt_printf(vt, "%-28s: %s\n", "rtk server state", svrstate[data->state]);
    vt_printf(vt, "%-28s: %d\n", "processing cycle (ms)", data->cycle);
    vt_printf(vt, "%-28s: %s\n", "positioning mode",
              rtk->opt.mode >= 0 && rtk->opt.mode < (int)(sizeof(mode) / sizeof(mode[0])) ? mode[rtk->opt.mode] : "?");
    vt_printf(vt, "%-28s: %s\n", "frequencies",
              rtk->opt.nf >= 0 && rtk->opt.nf < (int)(sizeof(freq) / sizeof(freq[0])) ? freq[rtk->opt.nf] : "?");
    vt_printf(vt, "%-28s: %02.0f:%02.0f:%04.1f\n", "accumulated time to run", rt[0], rt[1], rt[2]);
    vt_printf(vt, "%-28s: %d\n", "cpu time for a cycle (ms)", data->cputime);
    vt_printf(vt, "%-28s: %d\n", "missing obs data count", data->prcout);
    vt_printf(vt, "%-28s: %d,%d\n", "bytes in input buffer", nb[0], nb[1]);
    for (i = 0; i < 3; i++) {
        sprintf(s, "# of input%d data", i + 1);
        vt_printf(vt, "%-28s: obs(%d),nav(%d),gnav(%d),ion(%d),sbs(%d),pos(%d),dgps(%d),ssr(%d),err(%d)\n", s,
                  nmsg[i][0], nmsg[i][1], nmsg[i][6], nmsg[i][2], nmsg[i][3], nmsg[i][4], nmsg[i][5], nmsg[i][7],
                  nmsg[i][9]);
    }
    for (i = 0; i < 3; i++) {
        p = s;
        *p = '\0';
        for (j = 1; j < 100; j++) {
            if (nmsg2[i][j] == 0) {
                continue;
            }
            p += sprintf(p, "%s%d(%d)", p > s ? "," : "", j, nmsg2[i][j]);
        }
        if (nmsg2[i][0] > 0) {
            sprintf(p, "%sother2(%d)", p > s ? "," : "", nmsg2[i][0]);
        }
        for (j = 1; j < 300; j++) {
            if (nmsg3[i][j] == 0) {
                continue;
            }
            p += sprintf(p, "%s%d(%d)", p > s ? "," : "", j + 1000, nmsg3[i][j]);
        }
        if (nmsg3[i][0] > 0) {
            sprintf(p, "%sother3(%d)", p > s ? "," : "", nmsg3[i][0]);
        }
        sprintf(tmp, "# of rtcm%d messages", i + 1);
        vt_printf(vt, "%-28s: %s\n", tmp, s);
    }
    vt_printf(vt, "%-28s: %s\n", "solution status", sol[data->rtkstat]);
    time2str(rtk->sol.time, tstr, 9);
    vt_printf(vt, "%-28s: %s\n", "time of receiver clock rover", rtk->sol.time.time ? tstr : "-");
    vt_printf(vt, "%-28s: %.3f,%.3f,%.3f,%.3f\n", "time sys offset (ns)", rtk->sol.dtr[1] * 1e9, rtk->sol.dtr[2] * 1e9,
              rtk->sol.dtr[3] * 1e9, rtk->sol.dtr[4] * 1e9);
    vt_printf(vt, "%-28s: %.3f\n", "solution interval (s)", rtk->tt);
    vt_printf(vt, "%-28s: %.3f\n", "age of differential (s)", rtk->sol.age);
    vt_printf(vt, "%-28s: %.3f\n", "ratio for ar validation", rtk->sol.ratio);
    vt_printf(vt, "%-28s: %d\n", "# of satellites rover", data->nsat0);
    vt_printf(vt, "%-28s: %d\n", "# of satellites base", data->nsat1);
    vt_printf(vt, "%-28s: %d\n", "# of valid satellites", rtk->sol.ns);
    vt_printf(vt, "%-28s: %.1f,%.1f,%.1f,%.1f\n", "GDOP/PDOP/HDOP/VDOP", dop[0], dop[1], dop[2], dop[3]);
    vt_printf(vt, "%-28s: %d\n", "# of real estimated states", rtk->na);
    vt_printf(vt, "%-28s: %d\n", "# of all estimated states", rtk->nx);
    vt_printf(vt, "%-28s: %.3f,%.3f,%.3f\n", "pos xyz single (m) rover", rtk->sol.rr[0], rtk->sol.rr[1],
              rtk->sol.rr[2]);
    if (norm(rtk->sol.rr, 3) > 0.0) {
        ecef2pos(rtk->sol.rr, pos);
    } else {
        pos[0] = pos[1] = pos[2] = 0.0;
    }
    vt_printf(vt, "%-28s: %.8f,%.8f,%.3f\n", "pos llh single (deg,m) rover", pos[0] * R2D, pos[1] * R2D, pos[2]);
    ecef2enu(pos, rtk->sol.rr + 3, vel);
    vt_printf(vt, "%-28s: %.3f,%.3f,%.3f\n", "vel enu (m/s) rover", vel[0], vel[1], vel[2]);
    vt_printf(vt, "%-28s: %.3f,%.3f,%.3f\n", "pos xyz float (m) rover", data->has_x ? x3[0] : 0,
              data->has_x ? x3[1] : 0, data->has_x ? x3[2] : 0);
    vt_printf(vt, "%-28s: %.3f,%.3f,%.3f\n", "pos xyz float std (m) rover", data->has_P ? SQRT(P_diag[0]) : 0,
              data->has_P ? SQRT(P_diag[1]) : 0, data->has_P ? SQRT(P_diag[2]) : 0);
    vt_printf(vt, "%-28s: %.3f,%.3f,%.3f\n", "pos xyz fixed (m) rover", data->has_xa ? xa3[0] : 0,
              data->has_xa ? xa3[1] : 0, data->has_xa ? xa3[2] : 0);
    vt_printf(vt, "%-28s: %.3f,%.3f,%.3f\n", "pos xyz fixed std (m) rover", data->has_Pa ? SQRT(Pa_diag[0]) : 0,
              data->has_Pa ? SQRT(Pa_diag[1]) : 0, data->has_Pa ? SQRT(Pa_diag[2]) : 0);
    vt_printf(vt, "%-28s: %.3f,%.3f,%.3f\n", "pos xyz (m) base", rtk->rb[0], rtk->rb[1], rtk->rb[2]);
    if (norm(rtk->rb, 3) > 0.0) {
        ecef2pos(rtk->rb, pos);
    } else {
        pos[0] = pos[1] = pos[2] = 0.0;
    }
    vt_printf(vt, "%-28s: %.8f,%.8f,%.3f\n", "pos llh (deg,m) base", pos[0] * R2D, pos[1] * R2D, pos[2]);
    vt_printf(vt, "%-28s: %d\n", "# of average single pos base", data->nave);
    vt_printf(vt, "%-28s: %s\n", "ant type rover", rtk->opt.pcvr[0].type);
    del = rtk->opt.antdel[0];
    vt_printf(vt, "%-28s: %.3f %.3f %.3f\n", "ant delta rover", del[0], del[1], del[2]);
    vt_printf(vt, "%-28s: %s\n", "ant type base", rtk->opt.pcvr[1].type);
    del = rtk->opt.antdel[1];
    vt_printf(vt, "%-28s: %.3f %.3f %.3f\n", "ant delta base", del[0], del[1], del[2]);
    ecef2enu(pos, rtk->rb + 3, vel);
    vt_printf(vt, "%-28s: %.3f,%.3f,%.3f\n", "vel enu (m/s) base", vel[0], vel[1], vel[2]);
    if (rtk->opt.mode > 0 && data->has_x && norm(x3, 3) > 0.0) {
        for (i = 0; i < 3; i++) {
            rr[i] = x3[i] - rtk->rb[i];
        }
        bl1 = norm(rr, 3);
    }
    if (rtk->opt.mode > 0 && data->has_xa && norm(xa3, 3) > 0.0) {
        for (i = 0; i < 3; i++) {
            rr[i] = xa3[i] - rtk->rb[i];
        }
        bl2 = norm(rr, 3);
    }
    vt_printf(vt, "%-28s: %.3f\n", "baseline length float (m)", bl1);
    vt_printf(vt, "%-28s: %.3f\n", "baseline length fixed (m)", bl2);
    vt_printf(vt, "%-28s: %d\n", "monitor port", rcvcmd_monitor_port(cmd));
    free(data);
}
/* print satellite -----------------------------------------------------------*/
static void prsatellite(vt_t* vt, int sys, int nf) {
    rcvcmd_satellite_t* data;
    const rtk_t* rtk;
    double az, el;
    char id[32];
    int i, j, fix, frq[] = {1, 2, 5, 7, 8, 6};

    trace(NULL, 4, "prsatellite:\n");

    if (!(data = (rcvcmd_satellite_t*)malloc(sizeof(*data)))) {
        trace(NULL, 2, "prsatellite: snapshot allocation failed\n");
        return;
    }
    rtk = &data->rtk;

    rcvcmd_read_satellite(cmd, data);
    if (nf <= 0 || nf > NFREQ) {
        nf = NFREQ;
    }
    vt_printf(vt, "\n%s%3s %2s %5s %4s", ESC_BOLD, "SAT", "C1", "Az", "El");
    for (j = 0; j < nf; j++) {
        vt_printf(vt, " L%d", frq[j]);
    }
    for (j = 0; j < nf; j++) {
        vt_printf(vt, "  Fix%d", frq[j]);
    }
    for (j = 0; j < nf; j++) {
        vt_printf(vt, "  P%dRes", frq[j]);
    }
    for (j = 0; j < nf; j++) {
        vt_printf(vt, "   L%dRes", frq[j]);
    }
    for (j = 0; j < nf; j++) {
        vt_printf(vt, "  Sl%d", frq[j]);
    }
    for (j = 0; j < nf; j++) {
        vt_printf(vt, "  Lock%d", frq[j]);
    }
    for (j = 0; j < nf; j++) {
        vt_printf(vt, " Rj%d", frq[j]);
    }
    vt_printf(vt, "%s\n", ESC_RESET);

    for (i = 0; i < MAXSAT; i++) {
        if (rtk->ssat[i].azel[1] <= 0.0 || !(satsys(i + 1, NULL) & sys)) {
            continue;
        }
        satno2id(i + 1, id);
        vt_printf(vt, "%3s %2s", id, rtk->ssat[i].vs ? "OK" : "-");
        az = rtk->ssat[i].azel[0] * R2D;
        if (az < 0.0) {
            az += 360.0;
        }
        el = rtk->ssat[i].azel[1] * R2D;
        vt_printf(vt, " %5.1f %4.1f", az, el);
        for (j = 0; j < nf; j++) {
            vt_printf(vt, " %2s", rtk->ssat[i].vsat[j] ? "OK" : "-");
        }
        for (j = 0; j < nf; j++) {
            fix = rtk->ssat[i].fix[j];
            const char* fix_str;
            if (fix == 1) {
                fix_str = "FLOAT";
            } else if (fix == 2) {
                fix_str = "FIX";
            } else if (fix == 3) {
                fix_str = "HOLD";
            } else {
                fix_str = "-";
            }
            vt_printf(vt, " %5s", fix_str);
        }
        for (j = 0; j < nf; j++) {
            vt_printf(vt, "%7.3f", rtk->ssat[i].resp[j]);
        }
        for (j = 0; j < nf; j++) {
            vt_printf(vt, "%8.4f", rtk->ssat[i].resc[j]);
        }
        for (j = 0; j < nf; j++) {
            vt_printf(vt, " %4d", rtk->ssat[i].slipc[j]);
        }
        for (j = 0; j < nf; j++) {
            vt_printf(vt, " %6d", rtk->ssat[i].lock[j]);
        }
        for (j = 0; j < nf; j++) {
            vt_printf(vt, " %3d", rtk->ssat[i].rejc[j]);
        }
        vt_printf(vt, "\n");
    }
    free(data);
}
/* print observation data ----------------------------------------------------*/
static void probserv(vt_t* vt, int sys, int nf) {
    rcvcmd_observ_t* data;
    const obsd_t* obs;
    char tstr[64], id[32];
    int i, j, n = 0;

    trace(NULL, 4, "probserv:\n");

    if (!(data = (rcvcmd_observ_t*)malloc(sizeof(*data)))) {
        trace(NULL, 2, "probserv: snapshot allocation failed\n");
        return;
    }
    obs = data->obs;

    rcvcmd_read_observ(cmd, sys, data);
    n = data->n;

    if (nf <= 0 || nf > NFREQ) {
        nf = NFREQ;
    }
    vt_printf(vt, "\n%s%-22s %3s %s", ESC_BOLD, "      TIME(GPST)", "SAT", "R");
    for (i = 0; i < nf; i++) {
        vt_printf(vt, " C%d", i + 1);
    }
    for (i = 0; i < nf; i++) {
        vt_printf(vt, " S%d", i + 1);
    }
    for (i = 0; i < nf; i++) {
        vt_printf(vt, "        P%d(m)", i + 1);
    }
    for (i = 0; i < nf; i++) {
        vt_printf(vt, "       L%d(cyc)", i + 1);
    }
    for (i = 0; i < nf; i++) {
        vt_printf(vt, "  D%d(Hz)", i + 1);
    }
    vt_printf(vt, " LLI%s\n", ESC_RESET);
    for (i = 0; i < n; i++) {
        time2str(obs[i].time, tstr, 2);
        satno2id(obs[i].sat, id);
        vt_printf(vt, "%s %3s %d", tstr, id, obs[i].rcv);
        for (j = 0; j < nf; j++) {
            vt_printf(vt, "%3s", code2obs(obs[i].code[j]));
        }
        for (j = 0; j < nf; j++) {
            vt_printf(vt, "%3.0f", obs[i].SNR[j] * SNR_UNIT);
        }
        for (j = 0; j < nf; j++) {
            vt_printf(vt, "%13.3f", obs[i].P[j]);
        }
        for (j = 0; j < nf; j++) {
            vt_printf(vt, "%14.3f", obs[i].L[j]);
        }
        for (j = 0; j < nf; j++) {
            vt_printf(vt, "%8.1f", obs[i].D[j]);
        }
        vt_printf(vt, " ");
        for (j = 0; j < nf; j++) {
            vt_printf(vt, "%1X", obs[i].LLI[j]);
        }
        vt_printf(vt, "\n");
    }
    free(data);
}
/* print navigation data -----------------------------------------------------*/
static void prnavidata(vt_t* vt, int sys) {
    rcvcmd_navidata_t* data;
    const eph_t* eph;
    const geph_t* geph;
    const double *ion, *utc;
    gtime_t time;
    char id[32], s1[64], s2[64], s3[64];
    int i, valid, prn;

    trace(NULL, 4, "prnavidata:\n");

    if (!(data = (rcvcmd_navidata_t*)malloc(sizeof(*data)))) {
        trace(NULL, 2, "prnavidata: snapshot allocation failed\n");
        return;
    }
    eph = data->eph;
    geph = data->geph;
    ion = data->ion;
    utc = data->utc;

    rcvcmd_read_navidata(cmd, data);
    time = data->time;

    vt_printf(vt, "\n%s%3s %3s %3s %3s %3s %3s %3s %19s %19s %19s %3s %3s%s\n", ESC_BOLD, "SAT", "S", "IOD", "IOC",
              "FRQ", "A/A", "SVH", "Toe", "Toc", "Ttr/Tof", "L2C", "L2P", ESC_RESET);
    for (i = 0; i < MAXSAT * 2; i++) {
        if (!(satsys(i + 1, NULL) & sys) || eph[i].sat != (i + 1) % MAXSAT) {
            continue;
        }
        valid = eph[i].toe.time != 0 && !eph[i].svh && fabs(timediff(time, eph[i].toe)) <= MAXDTOE;
        satno2id(i + 1, id);
        if (eph[i].toe.time != 0) {
            time2str(eph[i].toe, s1, 0);
        } else {
            strcpy(s1, "-");
        }
        if (eph[i].toc.time != 0) {
            time2str(eph[i].toc, s2, 0);
        } else {
            strcpy(s2, "-");
        }
        if (eph[i].ttr.time != 0) {
            time2str(eph[i].ttr, s3, 0);
        } else {
            strcpy(s3, "-");
        }
        vt_printf(vt, "%3s %3s %3d %3d %3d %3d %03X %19s %19s %19s %3d %3d\n", id, valid ? "OK" : "-", eph[i].iode,
                  eph[i].iodc, 0, eph[i].sva, eph[i].svh, s1, s2, s3, eph[i].code, eph[i].flag);
    }
    for (i = 0; i < MAXSAT; i++) {
        if (!(satsys(i + 1, &prn) & SYS_GLO & sys) || geph[prn - 1].sat != i + 1) {
            continue;
        }
        valid =
            geph[prn - 1].toe.time != 0 && !geph[prn - 1].svh && fabs(timediff(time, geph[prn - 1].toe)) <= MAXDTOE_GLO;
        satno2id(i + 1, id);
        if (geph[prn - 1].toe.time != 0) {
            time2str(geph[prn - 1].toe, s1, 0);
        } else {
            strcpy(s1, "-");
        }
        if (geph[prn - 1].tof.time != 0) {
            time2str(geph[prn - 1].tof, s2, 0);
        } else {
            strcpy(s2, "-");
        }
        vt_printf(vt, "%3s %3s %3d %3d %3d %3d  %02X %19s %19s %19s %3d %3d\n", id, valid ? "OK" : "-",
                  geph[prn - 1].iode, 0, geph[prn - 1].frq, geph[prn - 1].age, geph[prn].svh, s1, "-", s2, 0, 0);
    }
    vt_printf(vt, "ION: %9.2E %9.2E %9.2E %9.2E %9.2E %9.2E %9.2E %9.2E\n", ion[0], ion[1], ion[2], ion[3], ion[4],
              ion[5], ion[6], ion[7]);
    vt_printf(vt, "UTC: %9.2E %9.2E %9.2E %9.2E  LEAPS: %.0f\n", utc[0], utc[1], utc[2], utc[3], utc[4]);
    free(data);
}
/* print error/warning messages ----------------------------------------------*/
static void prerror(vt_t* vt) {
    rcvcmd_error_t* data;

    trace(NULL, 4, "prerror:\n");

    if (!(data = (rcvcmd_error_t*)malloc(sizeof(*data)))) {
        trace(NULL, 2, "prerror: snapshot allocation failed\n");
        return;
    }

    rcvcmd_read_error(cmd, data);
    if (data->n > 0) {
        vt_puts(vt, data->errbuf);
    }
    free(data);
}
/* print stream --------------------------------------------------------------*/
static void prstream(vt_t* vt) {
    rcvcmd_stream_t* data;
    const char* ch[] = {"input1", "input2", "input3", "output1", "output2", "log1", "log2", "log3", "monitor"};
    const char* type[] = {"-",   "serial", "file",     "tcpsvr", "tcpcli", "ntripsvr", "ntripcli",
                          "ftp", "http",   "ntripcas", "udpsvr", "udpcli", ""};
    const char* fmt[] = {"rtcm2", "rtcm3", "oem4",  "oem3", "ubx",  "ss2", "hemis", "skytreq",
                         "javad", "nvs",   "binex", "rt17", "sbf",  "",    "sp3",   "",
                         "",      "",      "",      "",     "stat", "l6e", "clas"};
    const char* sol[] = {"llh", "xyz", "enu", "nmea", "stat", "-"};
    const stream_t* stream;
    const int* format;
    int i, read;

    trace(NULL, 4, "prstream:\n");

    if (!(data = (rcvcmd_stream_t*)malloc(sizeof(*data)))) {
        trace(NULL, 2, "prstream: snapshot allocation failed\n");
        return;
    }
    stream = data->stream;
    format = data->format;

    read = rcvcmd_read_stream(cmd, data);

    vt_printf(vt, "\n%s%-12s %-8s %-5s %s %10s %7s %10s %7s %-24s %s%s\n", ESC_BOLD, "Stream", "Type", "Fmt", "S",
              "In-byte", "In-bps", "Out-byte", "Out-bps", "Path", "Message", ESC_RESET);
    for (i = 0; read && i < 9; i++) {
        vt_printf(vt, "%-12s %-8s %-5s %s %10d %7d %10d %7d %-24.24s %s\n", ch[i], type[stream[i].type],
                  i < 3 ? fmt[format[i]] : (i < 5 || i == 8 ? sol[format[i]] : "-"),
                  stream[i].state < 0 ? "E" : (stream[i].state ? "C" : "-"), stream[i].inb, stream[i].inr,
                  stream[i].outb, stream[i].outr, stream[i].path, stream[i].msg);
    }
    free(data);
}
/* print ssr correction ------------------------------------------------------*/
static void prssr(vt_t* vt) {
    rcvcmd_ssr_t* data;
    gtime_t time;
    const ssr_t* ssr;
    int i, valid;
    char tstr[64], id[32];

    if (!(data = (rcvcmd_ssr_t*)malloc(sizeof(*data)))) {
        trace(NULL, 2, "prssr: snapshot allocation failed\n");
        return;
    }
    ssr = data->ssr;

    rcvcmd_read_ssr(cmd, data);
    time = data->time;

    /* Match the old vt_puts abort at the first failed write. */
    if (!vt_printf(vt,
                   "\n%s%3s %3s %3s %3s %3s %19s %6s %6s %6s %6s %6s %6s %8s "
                   "%6s %6s %6s%s\n",
                   ESC_BOLD, "SAT", "S", "UDI", "IOD", "URA", "T0", "D0-A", "D0-C", "D0-R", "D1-A", "D1-C", "D1-R",
                   "C0", "C1", "C2", "C-HR", ESC_RESET)) {
        free(data);
        return;
    }
    for (i = 0; i < MAXSAT; i++) {
        if (!ssr[i].t0[0].time) {
            continue;
        }
        satno2id(i + 1, id);
        valid = fabs(timediff(time, ssr[i].t0[0])) <= 1800.0;
        time2str(ssr[i].t0[0], tstr, 0);
        if (!vt_printf(vt,
                       "%3s %3s %3.0f %3d %3d %19s %6.3f %6.3f %6.3f %6.3f %6.3f "
                       "%6.3f %8.3f %6.3f %6.4f %6.3f\n",
                       id, valid ? "OK" : "-", ssr[i].udi[0], ssr[i].iode, ssr[i].ura, tstr, ssr[i].deph[0],
                       ssr[i].deph[1], ssr[i].deph[2], ssr[i].ddeph[0] * 1E3, ssr[i].ddeph[1] * 1E3,
                       ssr[i].ddeph[2] * 1E3, ssr[i].dclk[0], ssr[i].dclk[1] * 1E3, ssr[i].dclk[2] * 1E3,
                       ssr[i].hrclk)) {
            free(data);
            return;
        }
    }
    free(data);
}
/* parse system option -------------------------------------------------------*/
static int parse_sysopt(const char* arg) {
    const int syss[] = {SYS_GPS, SYS_GLO, SYS_GAL, SYS_QZS, SYS_CMP, SYS_IRN, SYS_SBS};
    const char *sys_str = "GREJCIS", *p;
    int i, sys = 0;

    if (arg[0] != '-') {
        return 0;
    }

    for (i = 1; arg[i]; i++) {
        if ((p = strchr(sys_str, arg[i]))) {
            sys |= syss[(int)(p - sys_str)];
        }
    }
    return sys;
}
/* lifecycle result ----------------------------------------------------------*/
static int lifecycle_result(vt_t* vt, rcvcmd_result_t result) {
    if (result.kind == RCVCMD_SHUTTING_DOWN) {
        vt_printf(vt, "rtk server shutting down\n");
    } else if (result.kind == RCVCMD_BUSY) {
        vt_printf(vt, "rtk server busy (%s)\n", result.state == RCVCMD_STARTING ? "starting" : "stopping");
    } else if (result.kind == RCVCMD_ALREADY_RUNNING) {
        vt_printf(vt, "rtk server start error (server already started)\n");
    }
    return result.kind == RCVCMD_SUCCESS;
}
/* start command -------------------------------------------------------------*/
static void cmd_start(char** args, int narg, vt_t* vt) {
    rcvcmd_io_t io = {NULL, console_notice, console_confirm_overwrite};
    trace(NULL, 3, "cmd_start:\n");
    io.arg = vt;

    if (!lifecycle_result(vt, rcvcmd_start(cmd, &io))) {
        return;
    }
    vt_printf(vt, "rtk server start\n");
}
/* stop command --------------------------------------------------------------*/
static void cmd_stop(char** args, int narg, vt_t* vt) {
    rcvcmd_io_t io = {NULL, console_notice, console_confirm_overwrite};
    trace(NULL, 3, "cmd_stop:\n");
    io.arg = vt;

    if (!lifecycle_result(vt, rcvcmd_stop(cmd, &io))) {
        return;
    }
    vt_printf(vt, "rtk server stop\n");
}
/* restart command -----------------------------------------------------------*/
static void cmd_restart(char** args, int narg, vt_t* vt) {
    rcvcmd_io_t io = {NULL, console_notice, console_confirm_overwrite};
    trace(NULL, 3, "cmd_restart:\n");
    io.arg = vt;

    if (!lifecycle_result(vt, rcvcmd_restart(cmd, &io))) {
        return;
    }
    vt_printf(vt, "rtk server restart\n");
}
/* solution command ----------------------------------------------------------*/
static void cmd_solution(char** args, int narg, vt_t* vt) {
    rcvcmd_solution_t* data;
    int i, cycle = 0;

    trace(NULL, 3, "cmd_solution:\n");

    if (narg > 1) {
        cycle = (int)(atof(args[1]) * 1000.0);
    }

    if (!(data = (rcvcmd_solution_t*)malloc(sizeof(*data)))) {
        trace(NULL, 2, "cmd_solution: snapshot allocation failed\n");
        return;
    }

    if (cycle > 0) {
        rcvcmd_clear_solution(cmd);
    }

    while (!vt_chkbrk(vt)) {
        rcvcmd_read_solution(cmd, data);
        for (i = 0; i < data->nsol; i++) {
            prsolution(vt, &data->solbuf[i], data->rb);
        }
        if (cycle > 0) {
            sleepms(cycle);
        } else {
            break;
        }
    }
    free(data);
}
/* status command ------------------------------------------------------------*/
static void cmd_status(char** args, int narg, vt_t* vt) {
    int cycle = 0;

    trace(NULL, 3, "cmd_status:\n");

    if (narg > 1) {
        cycle = (int)(atof(args[1]) * 1000.0);
    }

    while (!vt_chkbrk(vt)) {
        if (cycle > 0) {
            vt_printf(vt, ESC_CLEAR);
        }
        prstatus(vt);
        if (cycle > 0) {
            sleepms(cycle);
        } else {
            return;
        }
    }
    vt_printf(vt, "\n");
}
/* satellite command ---------------------------------------------------------*/
static void cmd_satellite(char** args, int narg, vt_t* vt) {
    int i, n, s, nf = 2, sys = SYS_ALL, cycle = 0;

    trace(NULL, 3, "cmd_satellite:\n");

    for (i = 1; i < narg; i++) {
        if (sscanf(args[i], "-%d", &n) == 1) {
            nf = n;
        } else if ((s = parse_sysopt(args[i]))) {
            sys = s;
        } else {
            cycle = (int)(atof(args[i]) * 1000.0);
        }
    }
    while (!vt_chkbrk(vt)) {
        if (cycle > 0) {
            vt_printf(vt, ESC_CLEAR);
        }
        prsatellite(vt, sys, nf);
        if (cycle > 0) {
            sleepms(cycle);
        } else {
            return;
        }
    }
    vt_printf(vt, "\n");
}
/* observ command ------------------------------------------------------------*/
static void cmd_observ(char** args, int narg, vt_t* vt) {
    int i, n, s, nf = 3, sys = SYS_ALL, cycle = 0;

    trace(NULL, 3, "cmd_observ:\n");

    for (i = 1; i < narg; i++) {
        if (sscanf(args[i], "-%d", &n) == 1) {
            nf = n;
        } else if ((s = parse_sysopt(args[i]))) {
            sys = s;
        } else {
            cycle = (int)(atof(args[i]) * 1000.0);
        }
    }
    while (!vt_chkbrk(vt)) {
        if (cycle > 0) {
            vt_printf(vt, ESC_CLEAR);
        }
        probserv(vt, sys, nf);
        if (cycle > 0) {
            sleepms(cycle);
        } else {
            return;
        }
    }
    vt_printf(vt, "\n");
}
/* navidata command ----------------------------------------------------------*/
static void cmd_navidata(char** args, int narg, vt_t* vt) {
    int i, s, sys = SYS_ALL, cycle = 0;

    trace(NULL, 3, "cmd_navidata:\n");

    for (i = 1; i < narg; i++) {
        if ((s = parse_sysopt(args[i]))) {
            sys = s;
        } else {
            cycle = (int)(atof(args[1]) * 1000.0);
        }
    }
    while (!vt_chkbrk(vt)) {
        if (cycle > 0) {
            vt_printf(vt, ESC_CLEAR);
        }
        prnavidata(vt, sys);
        if (cycle > 0) {
            sleepms(cycle);
        } else {
            return;
        }
    }
    vt_printf(vt, "\n");
}
/* error command -------------------------------------------------------------*/
static void cmd_error(char** args, int narg, vt_t* vt) {
    trace(NULL, 3, "cmd_error:\n");

    rcvcmd_clear_error(cmd);

    while (!vt_chkbrk(vt)) {
        prerror(vt);
        sleepms(100);
    }
    vt_printf(vt, "\n");
}
/* stream command ------------------------------------------------------------*/
static void cmd_stream(char** args, int narg, vt_t* vt) {
    int cycle = 0;

    trace(NULL, 3, "cmd_stream:\n");

    if (narg > 1) {
        cycle = (int)(atof(args[1]) * 1000.0);
    }

    while (!vt_chkbrk(vt)) {
        if (cycle > 0) {
            vt_printf(vt, ESC_CLEAR);
        }
        prstream(vt);
        if (cycle > 0) {
            sleepms(cycle);
        } else {
            return;
        }
    }
    vt_printf(vt, "\n");
}
/* ssr command ---------------------------------------------------------------*/
static void cmd_ssr(char** args, int narg, vt_t* vt) {
    int cycle = 0;

    trace(NULL, 3, "cmd_ssr:\n");

    if (narg > 1) {
        cycle = (int)(atof(args[1]) * 1000.0);
    }

    while (!vt_chkbrk(vt)) {
        if (cycle > 0) {
            vt_printf(vt, ESC_CLEAR);
        }
        prssr(vt);
        if (cycle > 0) {
            sleepms(cycle);
        } else {
            return;
        }
    }
    vt_printf(vt, "\n");
}
/* option command ------------------------------------------------------------*/
static void cmd_option(char** args, int narg, vt_t* vt) {
    const opt_t* opts[2];
    char *listing, *p;
    size_t size = 1;
    int i, system;

    trace(NULL, 3, "cmd_option:\n");
    opts[0] = rcvcmd_options(cmd, 0);
    opts[1] = rcvcmd_options(cmd, 1);

    /* String values fit MAXSTR; enum values also come from the comments. */
    for (system = 0; system < 2; system++) {
        for (i = 0; *opts[system][i].name; i++) {
            size += MAXSTR + strlen(opts[system][i].name) + 2 * strlen(opts[system][i].comment) + 32;
        }
    }
    if (!(listing = (char*)malloc(size))) {
        trace(NULL, 2, "cmd_option: snapshot allocation failed\n");
        return;
    }

    rcvcmd_lock_options(cmd);
    p = listing;
    *p = '\0';
    for (system = 0; system < 2; system++) {
        for (i = 0; *opts[system][i].name; i++) {
            if (narg >= 2 && !strstr(opts[system][i].name, args[1])) {
                continue;
            }
            *p++ = rcvcmd_option_modified(cmd, system, i) ? '*' : ' ';
            p += opt2buf(opts[system] + i, p);
            *p++ = '\n';
            *p = '\0';
        }
    }
    rcvcmd_unlock_options(cmd);

    vt_puts(vt, listing);
    free(listing);
}
/* set command ---------------------------------------------------------------*/
static void cmd_set(char** args, int narg, vt_t* vt) {
    opt_t* opt;
    rcvcmd_option_t option;
    rcvcmd_config_result_t result;
    char buff[MAXSTR];

    trace(NULL, 3, "cmd_set:\n");

    if (narg < 2) {
        vt_printf(vt, "specify option type\n");
        return;
    }
    if (!rcvcmd_find_option(cmd, args[1], &option)) {
        vt_printf(vt, "no option type: %s\n", args[1]);
        return;
    }
    opt = option.opt;
    if (narg < 3) {
        if (!rcvcmd_can_set_option(cmd, &option)) {
            vt_printf(vt, "option not changed: stop rtk server first\n");
            return;
        }
        vt_printf(vt, "%s", opt->name);
        if (*opt->comment) {
            vt_printf(vt, " (%s)", opt->comment);
        }
        vt_printf(vt, ": ");
        if (!vt_gets(vt, buff, sizeof(buff)) || vt->brk) {
            return;
        }
    } else {
        strcpy(buff, args[2]);
    }

    chop(buff);
    result = rcvcmd_set_option(cmd, &option, buff);
    if (result == RCVCMD_CONFIG_REFUSED) {
        vt_printf(vt, "option not changed: stop rtk server first\n");
        return;
    }
    if (result == RCVCMD_CONFIG_ERROR) {
        vt_printf(vt, "invalid option value: %s %s\n", opt->name, buff);
        return;
    }

    vt_printf(vt, "option %s changed.", opt->name);
    if (rcvcmd_option_requires_restart(&option)) {
        vt_printf(vt, " restart to enable it");
    }
    vt_printf(vt, "\n");
}
/* load command --------------------------------------------------------------*/
static void cmd_load(char** args, int narg, vt_t* vt) {
    char file[MAXSTR] = "";
    rcvcmd_config_result_t result;

    trace(NULL, 3, "cmd_load:\n");

    if (narg >= 2) {
        strcpy(file, args[1]);
    } else {
        sprintf(file, "%s/%s", OPTSDIR, OPTSFILE);
    }
    result = rcvcmd_load(cmd, file);
    if (result == RCVCMD_CONFIG_REFUSED) {
        vt_printf(vt, "options not loaded: stop rtk server first\n");
        return;
    }
    if (result == RCVCMD_CONFIG_ERROR) {
        vt_printf(vt, "no options file: %s\n", file);
        return;
    }
    vt_printf(vt, "options loaded from %s. restart to enable them\n", file);
}
/* save command --------------------------------------------------------------*/
static void cmd_save(char** args, int narg, vt_t* vt) {
    char file[MAXSTR] = "";

    trace(NULL, 3, "cmd_save:\n");

    if (narg >= 2) {
        strcpy(file, args[1]);
    } else {
        sprintf(file, "%s/%s", OPTSDIR, OPTSFILE);
    }
    if (!confwrite(vt, file)) {
        return;
    }
    if (!rcvcmd_save(cmd, file)) {
        vt_printf(vt, "options save error: %s\n", file);
        return;
    }
    vt_printf(vt, "options saved to %s\n", file);
}
/* log command ---------------------------------------------------------------*/
static void cmd_log(char** args, int narg, vt_t* vt) {
    trace(NULL, 3, "cmd_log:\n");

    if (narg < 2) {
        vt_printf(vt, "specify log file\n");
        return;
    }
    if (!strcmp(args[1], "off")) {
        vt_closelog(vt);
        vt_printf(vt, "log off\n");
        return;
    }
    if (!confwrite(vt, args[1])) {
        return;
    }

    if (!vt_openlog(vt, args[1])) {
        vt_printf(vt, "log open error: %s\n", args[1]);
        return;
    }
    vt_printf(vt, "log on: %s\n", args[1]);
}
/* help command --------------------------------------------------------------*/
static void cmd_help(char** args, int narg, vt_t* vt) {
    char str[] = "path";
    int i;

    if (narg < 2) {
        vt_printf(vt, "%s (%s ver.%s git %s)\n", PRGNAME, MRTKLIB_SOFTNAME, MRTKLIB_VERSION_STRING,
                  mrtklib_git_hash_str);
        for (i = 0; *helptxt[i]; i++) {
            vt_printf(vt, "%s\n", helptxt[i]);
        }
    } else if (strstr(str, args[1]) == str) {
        for (i = 0; *pathopts[i]; i++) {
            vt_printf(vt, "%s\n", pathopts[i]);
        }
    } else {
        vt_printf(vt, "unknown help: %s\n", args[1]);
    }
}
/* exec command --------------------------------------------------------------*/
static int cmd_exec(const char* cmd, vt_t* vt) {
    FILE* fp;
    int ret;
    char buff[MAXSTR];

    if (!(fp = popen(cmd, "r"))) {
        vt_printf(vt, "command exec error\n");
        return -1;
    }
    while (!vt_chkbrk(vt)) {
        if (!fgets(buff, sizeof(buff), fp)) {
            break;
        }
        vt_printf(vt, buff);
    }
    if ((ret = pclose(fp))) {
        vt_printf(vt, "command exec error (%d)\n", ret);
    }
    return ret;
}
/* console thread ------------------------------------------------------------*/
static void* con_thread(void* arg) {
    const char* cmds[] = {"start",    "stop",   "restart", "solution", "status", "satellite", "observ",
                          "navidata", "stream", "ssr",     "error",    "option", "set",       "load",
                          "save",     "log",    "help",    "?",        "exit",   "shutdown",  ""};
    con_t* con = (con_t*)arg;
    int i, j, narg;
    char buff[MAXCMD], *args[MAXARG], *p;

    trace(NULL, 3, "console_thread:\n");

    vt_printf(con->vt, "\n%s** %s(%s ver.%s git %s) console (h:help) **%s\n", ESC_BOLD, PRGNAME, MRTKLIB_SOFTNAME,
              MRTKLIB_VERSION_STRING, mrtklib_git_hash_str, ESC_RESET);

    if (!login(con->vt)) {
        vt_close(con->vt);
        con->state = 0;
        return 0;
    }
    while (con->state) {
        /* output prompt */
        if (!vt_puts(con->vt, CMDPROMPT)) {
            break;
        }

        /* input command */
        if (!vt_gets(con->vt, buff, sizeof(buff))) {
            break;
        }

        if (buff[0] == '!') { /* shell escape */
            cmd_exec(buff + 1, con->vt);
            continue;
        }
        /* parse command */
        narg = 0;
        for (p = strtok(buff, " \t\n"); p && narg < MAXARG; p = strtok(NULL, " \t\n")) {
            args[narg++] = p;
        }
        if (narg == 0) {
            continue;
        }

        for (i = 0, j = -1; *cmds[i]; i++) {
            if (strstr(cmds[i], args[0]) == cmds[i]) {
                j = i;
            }
        }
        switch (j) {
            case 0:
                cmd_start(args, narg, con->vt);
                break;
            case 1:
                cmd_stop(args, narg, con->vt);
                break;
            case 2:
                cmd_restart(args, narg, con->vt);
                break;
            case 3:
                cmd_solution(args, narg, con->vt);
                break;
            case 4:
                cmd_status(args, narg, con->vt);
                break;
            case 5:
                cmd_satellite(args, narg, con->vt);
                break;
            case 6:
                cmd_observ(args, narg, con->vt);
                break;
            case 7:
                cmd_navidata(args, narg, con->vt);
                break;
            case 8:
                cmd_stream(args, narg, con->vt);
                break;
            case 9:
                cmd_ssr(args, narg, con->vt);
                break;
            case 10:
                cmd_error(args, narg, con->vt);
                break;
            case 11:
                cmd_option(args, narg, con->vt);
                break;
            case 12:
                cmd_set(args, narg, con->vt);
                break;
            case 13:
                cmd_load(args, narg, con->vt);
                break;
            case 14:
                cmd_save(args, narg, con->vt);
                break;
            case 15:
                cmd_log(args, narg, con->vt);
                break;
            case 16:
                cmd_help(args, narg, con->vt);
                break;
            case 17:
                cmd_help(args, narg, con->vt);
                break;
            case 18: /* exit */
                if (con->vt->type) {
                    con->state = 0;
                }
                break;
            case 19: /* shutdown */
                if (!strcmp(args[0], "shutdown")) {
                    /* Record after the reply has had time to reach the client:
                     * main does not wait for console threads once it sees the
                     * request (same order as the former intflg). */
                    vt_printf(con->vt, "rtk server shutdown ...\n");
                    sleepms(1000);
                    rcvcmd_request_shutdown(cmd);
                    con->state = 0;
                }
                break;
            default:
                vt_printf(con->vt, "unknown command: %s.\n", args[0]);
                break;
        }
    }
    vt_close(con->vt);
    return 0;
}
/* open console --------------------------------------------------------------*/
static con_t* con_open(int sock, const char* dev) {
    con_t* con;

    trace(NULL, 3, "con_open: sock=%d dev=%s\n", sock, dev);

    if (!(con = (con_t*)malloc(sizeof(con_t)))) {
        return NULL;
    }

    if (!(con->vt = vt_open(sock, dev))) {
        free(con);
        return NULL;
    }
    /* start console thread */
    con->state = 1;
    if (pthread_create(&con->thread, NULL, con_thread, con)) {
        free(con);
        return NULL;
    }
    return con;
}
/* close console -------------------------------------------------------------*/
static void con_close(con_t* con) {
    trace(NULL, 3, "con_close:\n");

    if (!con) {
        return;
    }
    con->state = 0;
    pthread_join(con->thread, NULL);
    free(con);
}
/* open socket for remote console --------------------------------------------*/
static int open_sock(int port) {
    struct sockaddr_in addr;
    int sock, on = 1;

    trace(NULL, 3, "open_sock: port=%d\n", port);

    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        fprintf(stderr, "socket error (%d)\n", errno);
        return 0;
    }
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof(on));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "bind error (%d)\n", errno);
        close(sock);
        return -1;
    }
    listen(sock, 5);
    return sock;
}
/* accept remote console connection ------------------------------------------*/
static void accept_sock(int ssock, con_t** con) {
    struct timeval tv = {0};
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    fd_set rs;
    int i, sock;

    if (ssock <= 0) {
        return;
    }

    trace(NULL, 4, "accept_sock: ssock=%d\n", ssock);

    for (i = 1; i < MAXCON; i++) {
        if (!con[i] || con[i]->state) {
            continue;
        }
        con_close(con[i]);
        con[i] = NULL;
    }
    FD_ZERO(&rs);
    FD_SET(ssock, &rs);
    if (select(ssock + 1, &rs, NULL, NULL, &tv) <= 0) {
        return;
    }
    if ((sock = accept(ssock, (struct sockaddr*)&addr, &len)) <= 0) {
        return;
    }
    for (i = 1; i < MAXCON; i++) {
        if (con[i]) {
            continue;
        }

        con[i] = con_open(sock, "");

        trace(NULL, 3, "remote console connected: addr=%s\n", inet_ntoa(addr.sin_addr));
        return;
    }
    close(sock);
    trace(NULL, 2, "remote console connection refused. addr=%s\n", inet_ntoa(addr.sin_addr));
}
/* rtkrcv main -----------------------------------------------------------------
 * Synopsis
 *     rtkrcv [-s][-p port][-d dev][-o file][-r level][-t level][-sta sta]
 *
 * Description
 *     A command line version of the real-time positioning AP by rtklib. To start
 *     or stop RTK server, to configure options or to print solution/status,
 *     login a console and input commands. As default, /dev/tty is used for the
 *     console. Use -p option for network login with telnet protocol. To show
 *     the available commands, type ? or help on the console. With -p option,
 *     multiple telnet console logins are allowed. The initial processing options
 *     are loaded from default file rtkrcv.conf. To change the file, use -o
 *     option. To configure the processing options, edit the options file or use
 *     set, load or save command on the console. To shutdown the program, use
 *     shutdown command on the console or send USR2 signal to the process.
 *
 * Option
 *     -s         start RTK server on program startup
 *     -p port    port number for telnet console
 *     -m port    port number for monitor stream
 *     -d dev     terminal device for console
 *     -o file    processing options file
 *     -w pwd     login password for remote console ("": no password)
 *     -r level   output solution status file (0:off,1:states,2:residuals)
 *     -t level   debug trace level (0:off,1-5:on)
 *     -sta sta   station name for receiver dcb
 *     -v|-ver    print version
 *
 * Command
 *     start
 *       Start RTK server. No need the command if the program runs with -s
 *       option.
 *
 *     stop
 *       Stop RTK server.
 *
 *     restart
 *       Stop and start RTK server. When stopped, start it.
 *
 *     solution [cycle]
 *       Show solutions. Without option, only one solution is shown. With
 *       option, the soluiton is displayed at intervals of cycle (s). To stop
 *       cyclic display, send break (ctr-C).
 *
 *     status [cycle]
 *       Show RTK status. Use option cycle for cyclic display.
 *
 *     satellite [-n] [cycle]
 *       Show satellite status. Use option cycle for cyclic display. Option -n
 *       specify number of frequencies.
 *
 *     observ [-n] [cycle]
 *       Show observation data. Use option cycle for cyclic display. Option -n
 *       specify number of frequencies.
 *
 *     navidata [cycle]
 *       Show navigation data. Use option cycle for cyclic display.
 *
 *     stream [cycle]
 *       Show stream status. Use option cycle for cyclic display.
 *
 *     error
 *       Show error/warning messages. To stop messages, send break (ctr-C).
 *
 *     option [opt]
 *       Show the values of processing options. Without option, all options are
 *       displayed. With option, only pattern-matched options are displayed.
 *
 *     set opt [val]
 *       Set an option to val, or prompt for the value if omitted. Stop the
 *       server first for non-console options; console-* changes are immediate.
 *       Apply stopped processing-option changes with start or restart.
 *
 *     load [file]
 *       Load processing options while stopped. Without option, default file
 *       rtkrcv.conf is used. Apply the changes with start or restart.
 *
 *     save [file]
 *       Save current processing optons to file. Without option, default file
 *       rtkrcv.conf is used.
 *
 *     log [file|off]
 *       Record console log to file. To stop recording the log, use option off.
 *
 *     help|? [path]
 *       Show the command list. With option path, the stream path options are
 *       shown.
 *
 *     exit
 *       Exit and logout console. The status of RTK server is not affected by
 *       the command.
 *
 *     shutdown
 *       Record shutdown after the reply, finish any current transition, then
 *       stop RTK server and exit. Further start/stop/restart commands are refused.
 *
 *     !command [arg...]
 *       Execute command by the operating system shell. Do not use the
 *       interactive command.
 *
 * Notes
 *     start/stop/restart report busy while a lifecycle transition is in progress.
 *     Transition displays keep their format with zero values or headers only.
 *     Short form of a command is allowed. In case of the short form, the
 *     command is distinguished according to header characters.
 *
 *-----------------------------------------------------------------------------*/
int mrtk_run(int argc, char** argv) {
    con_t* con[MAXCON] = {0};
    int i, start = 0, port = 0, outstat = 0, trace_level = 0, sock = 0, moniport = 0;
    char *dev = "", file[MAXSTR] = "";
    double ee[] = {2000, 12, 31, 23, 59, 59};
    char staname[32] = "", password[MAXSTR] = "admin";
    gtime_t rst = {0};

    mrtk_ctx_t* ctx;

    /* translate --long flags to their -short aliases before parsing */
    mrtk_normalize_args(argc, argv, opt_aliases);

    for (i = 1; i < argc; i++) {
        if (mrtk_is_help_flag(argv[i])) {
            printusage();
        } else if (!strcmp(argv[i], "-s")) {
            start = 1;
        } else if (!strcmp(argv[i], "-p") && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-m") && i + 1 < argc) {
            moniport = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-d") && i + 1 < argc) {
            dev = argv[++i];
        } else if (!strcmp(argv[i], "-o") && i + 1 < argc) {
            strcpy(file, argv[++i]);
        } else if (!strcmp(argv[i], "-w") && i + 1 < argc) {
            strcpy(password, argv[++i]);
        } else if (!strcmp(argv[i], "-r") && i + 1 < argc) {
            outstat = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-t") && i + 1 < argc) {
            trace_level = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-sta") && i + 1 < argc) {
            strcpy(staname, argv[++i]);
        } else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "-ver")) {
            printver();
        } else if (!strcmp(argv[i], "-rst") && i + 2 < argc) {
            sscanf(argv[++i], "%lf/%lf/%lf", ee, ee + 1, ee + 2);
            sscanf(argv[++i], "%lf:%lf:%lf", ee + 3, ee + 4, ee + 5);
            rst = epoch2time(ee);
        } else {
            printusage();
        }
    }

    if (!(cmd = rcvcmd_create())) {
        return -1;
    }

    /* Initialize MRTKLIB runtime context */
    ctx = mrtk_ctx_create();
    if (!ctx) {
        fprintf(stderr, "error: MRTKLIB context allocation failed\n");
        rcvcmd_destroy(cmd);
        return -1;
    }
    g_mrtk_ctx = ctx;
    g_mrtk_legacy_ctx = mrtk_context_new();
    if (!g_mrtk_legacy_ctx) {
        fprintf(stderr, "error: MRTKLIB legacy context allocation failed\n");
        mrtk_ctx_destroy(ctx);
        g_mrtk_ctx = NULL;
        rcvcmd_destroy(cmd);
        return -1;
    }

    if (trace_level > 0) {
        traceopen(ctx, TRACEFILE);
        tracelevel(ctx, trace_level);
    }
    rcvcmd_initialize(cmd, file, password, staname, moniport, outstat, rst);
    if (port) {
        /* open socket for remote console */
        if ((sock = open_sock(port)) <= 0) {
            fprintf(stderr, "console open error port=%d\n", port);
            rcvcmd_close(cmd);
            traceclose(ctx);
            mrtk_context_free(g_mrtk_legacy_ctx);
            g_mrtk_ctx = NULL;
            mrtk_ctx_destroy(ctx);
            rcvcmd_destroy(cmd);
            return -1;
        }
    } else {
        /* open device for local console */
        if (!(con[0] = con_open(0, dev))) {
            fprintf(stderr, "console open error dev=%s\n", dev);
            rcvcmd_close(cmd);
            traceclose(ctx);
            mrtk_context_free(g_mrtk_legacy_ctx);
            g_mrtk_ctx = NULL;
            mrtk_ctx_destroy(ctx);
            rcvcmd_destroy(cmd);
            return -1;
        }
    }
    signal(SIGINT, sigshut);  /* keyboard interrupt */
    signal(SIGTERM, sigshut); /* external shutdown signal */
    signal(SIGUSR2, sigshut);
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    {
        struct sigaction sa_crash;
        /* pre-build default-handler struct for use inside sigcrash() */
        sigemptyset(&sa_default.sa_mask);
        sa_default.sa_handler = SIG_DFL;
        sa_default.sa_flags = 0;
        /* register crash handler */
        sigemptyset(&sa_crash.sa_mask);
        sa_crash.sa_handler = sigcrash;
        sa_crash.sa_flags = 0;
        sigaction(SIGSEGV, &sa_crash, NULL);
    }

    /* start rtk server */
    if (start) {
        rcvcmd_start(cmd, &process_io);
    }
    while (!intflg && !rcvcmd_shutdown_requested(cmd)) {
        /* accept remote console connection */
        accept_sock(sock, con);
        sleepms(100);
    }
    if (intflg) {
        trace(NULL, 3, "sigshut: sig=%d\n", (int)shut_signal);
    }
    /* stop rtk server */
    rcvcmd_request_shutdown(cmd);
    rcvcmd_shutdown(cmd, &process_io);

    if (port) {
        /* close socket for remote console */
        close(sock);
    } else {
        /* close consoles */
        for (i = 0; i < MAXCON; i++) {
            con_close(con[i]);
        }
    }
    rcvcmd_close(cmd);
    rcvcmd_save_navigation(cmd);
    traceclose(ctx);
    mrtk_context_free(g_mrtk_legacy_ctx);
    g_mrtk_ctx = NULL;
    mrtk_ctx_destroy(ctx);
    rcvcmd_destroy(cmd);
    return 0;
}
