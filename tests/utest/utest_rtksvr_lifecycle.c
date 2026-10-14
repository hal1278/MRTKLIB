/*------------------------------------------------------------------------------
 * unit test : RTK server start state, immediate stop, failure cleanup and restart
 * Uses public APIs and fields only, with no test hooks in the library.
 *-----------------------------------------------------------------------------*/
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include "mrtklib/rtklib.h"

#define REPEAT 200
#define WATCHDOG_MS 5000

static const char* case_name;
static int iteration = 1, fails = 0;
static struct {
    pthread_mutex_t lock;
    int iteration, done;
    const char* operation;
    uint32_t tick;
} watchdog = {PTHREAD_MUTEX_INITIALIZER, 1, 0, "initialization", 0};

typedef struct {
    int strs[8], formats[3];
    char *paths[8], *cmds[3], *rcvopts[3];
    double nmeapos[3];
    prcopt_t prcopt;
    solopt_t solopt[2];
} options_t;

#define CHECK(cond, msg)                                                                 \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            fprintf(stderr, "FAIL: %s iteration %d: %s\n", case_name, iteration, (msg)); \
            fails++;                                                                     \
        }                                                                                \
    } while (0)

static void progress(const char* operation) {
    pthread_mutex_lock(&watchdog.lock);
    watchdog.iteration = iteration;
    watchdog.operation = operation;
    watchdog.tick = tickget();
    pthread_mutex_unlock(&watchdog.lock);
}

static void* watch(void* arg) {
    (void)arg;
    for (;;) {
        sleepms(100);
        pthread_mutex_lock(&watchdog.lock);
        if (watchdog.done) {
            pthread_mutex_unlock(&watchdog.lock);
            return NULL;
        }
        if ((uint32_t)(tickget() - watchdog.tick) >= WATCHDOG_MS) {
            fprintf(stderr, "FAIL: %s iteration %d: watchdog expired during %s (%d ms)\n", case_name,
                    watchdog.iteration, watchdog.operation, WATCHDOG_MS);
            fflush(stderr);
            _exit(2);
        }
        pthread_mutex_unlock(&watchdog.lock);
    }
}

static int state(rtksvr_t* svr) {
    int value;
    rtksvrlock(svr);
    value = svr->state;
    rtksvrunlock(svr);
    return value;
}

static int start(rtksvr_t* svr, options_t* opt, char* errmsg) {
    gtime_t rst = {0};
    errmsg[0] = '\0';
    return rtksvrstart(svr, 1, 4096, opt->strs, opt->paths, opt->formats, 0, opt->cmds, opt->cmds, opt->rcvopts, 0, 0,
                       opt->nmeapos, &opt->prcopt, opt->solopt, NULL, rst, errmsg);
}

static int start_ok(rtksvr_t* svr, options_t* opt) {
    char errmsg[MAXSTRPATH + 64];
    if (start(svr, opt, errmsg)) return 1;
    fprintf(stderr, "FAIL: %s iteration %d: start failed: %s\n", case_name, iteration, errmsg);
    fails++;
    return 0;
}

/* Isolate cleanup/repeated-stop checks from the immediate-stop race. */
static void wait_running(rtksvr_t* svr) {
    progress("waiting for running state");
    while (!state(svr)) sleepms(1);
}

static void test_start_state(rtksvr_t* svr, options_t* opt) {
    char errmsg[MAXSTRPATH + 64];
    int initial, second;
    for (iteration = 1; iteration <= REPEAT; iteration++) {
        progress("start and immediate checks");
        if (!start_ok(svr, opt)) return;
        initial = state(svr);
        second = start(svr, opt, errmsg);
        CHECK(initial == 1, "state immediately after start is not 1");
        if (second) {
            fprintf(stderr, "FAIL: %s iteration %d: second start was accepted\n", case_name, iteration);
            fflush(stderr);
            _exit(1); /* Two threads may own the same buffers; do not stop them. */
        }
        CHECK(strcmp(errmsg, "server already started") == 0, "second start error is not server already started");
        progress("stop");
        rtksvrstop(svr, opt->cmds);
        CHECK(state(svr) == 0, "state after stop is not 0");
    }
}

static void test_stop_after_start(rtksvr_t* svr, options_t* opt) {
    for (iteration = 1; iteration <= REPEAT; iteration++) {
        progress("start/stop immediately");
        if (!start_ok(svr, opt)) return;
        rtksvrstop(svr, opt->cmds);
        CHECK(state(svr) == 0, "state after stop is not 0");
    }
}

static void check_null(const void* ptr, const char* member, int i) {
    if (!ptr) return;
    fprintf(stderr, "FAIL: %s iteration %d: %s[%d] is not NULL\n", case_name, iteration, member, i);
    fails++;
}

static void test_start_failure_cleanup(rtksvr_t* svr, options_t* opt, char* path) {
    char badpath[MAXSTRPATH], errmsg[MAXSTRPATH + 64], expected[MAXSTRPATH + 64];
    stream_t probe;
    int i, result, n;

    n = snprintf(badpath, sizeof(badpath), "%s/sub/out.pos", path);
    CHECK(n >= 0 && n < (int)sizeof(badpath), "failure path is too long");
    if (fails) return;
    progress("checking file paths");
    strinit(&probe);
    result = stropen(&probe, STR_FILE, STR_MODE_R, path);
    CHECK(result, "existing input file cannot be opened");
    strclose(&probe);
    result = stropen(&probe, STR_FILE, STR_MODE_W, badpath);
    CHECK(!result, "output path unexpectedly opened through a regular file");
    strclose(&probe);
    pthread_mutex_destroy(&probe.lock);
    if (fails) return;

    opt->strs[0] = opt->strs[3] = STR_FILE;
    opt->paths[0] = path;
    opt->paths[3] = badpath;
    progress("start with invalid output path");
    result = start(svr, opt, errmsg);
    CHECK(!result, "start with invalid output path succeeded");
    if (result) {
        wait_running(svr);
        progress("stop unexpected successful start");
        rtksvrstop(svr, opt->cmds);
        return;
    }
    snprintf(expected, sizeof(expected), "str4 open error path=%s", badpath);
    CHECK(strcmp(errmsg, expected) == 0, "start did not fail at output stream 3");
    printf("start error: %s\n", errmsg);
    CHECK(state(svr) == 0, "state after failed start is not 0");
    /* The stream that failed to open holds no port and keeps its error state
     * (-1) for status display, as before; the streams opened before it are closed. */
    for (i = 0; i < 8; i++) {
        if (svr->stream[i].state != (i == 3 ? -1 : 0)) {
            fprintf(stderr, "FAIL: %s iteration %d: stream[%d].state is %d (expected %d)\n", case_name, iteration, i,
                    svr->stream[i].state, i == 3 ? -1 : 0);
            fails++;
        }
    }
    CHECK(svr->stream[3].port == NULL, "failed stream 3 holds a port");
    for (i = 0; i < 3; i++) {
        check_null(svr->buff[i], "buff", i);
        check_null(svr->pbuf[i], "pbuf", i);
        check_null(svr->raw[i].obs.data, "raw.obs.data", i);
        check_null(svr->rtcm[i].obs.data, "rtcm.obs.data", i);
    }
    for (i = 0; i < 2; i++) check_null(svr->sbuf[i], "sbuf", i);

    opt->strs[0] = opt->strs[3] = STR_NONE;
    opt->paths[0] = opt->paths[3] = "";
    progress("valid restart after failed start");
    if (!start_ok(svr, opt)) return;
    wait_running(svr);
    progress("stop after valid restart");
    rtksvrstop(svr, opt->cmds);
    CHECK(state(svr) == 0, "state after restart/stop is not 0");
}

static void test_stop_without_start(rtksvr_t* svr, options_t* opt) {
    CHECK(state(svr) == 0, "initial state is not 0");
    progress("stop without start");
    rtksvrstop(svr, opt->cmds);
    CHECK(state(svr) == 0, "state after stop without start is not 0");
}

static void test_double_stop(rtksvr_t* svr, options_t* opt) {
    progress("first start");
    if (!start_ok(svr, opt)) return;
    wait_running(svr);
    progress("first stop");
    rtksvrstop(svr, opt->cmds);
    CHECK(state(svr) == 0, "state after first stop is not 0");
    progress("second stop");
    rtksvrstop(svr, opt->cmds);
    CHECK(state(svr) == 0, "state after second stop is not 0");
    progress("restart after double stop");
    if (!start_ok(svr, opt)) return;
    wait_running(svr);
    progress("stop after restart");
    rtksvrstop(svr, opt->cmds);
    CHECK(state(svr) == 0, "state after restart/stop is not 0");
}

int main(int argc, char** argv) {
    rtksvr_t* svr;
    options_t opt = {0};
    pthread_t thread;
    int i, rc;

    if (argc != 3 || (strcmp(argv[1], "start_state") && strcmp(argv[1], "stop_after_start") &&
                      strcmp(argv[1], "start_failure_cleanup") && strcmp(argv[1], "stop_without_start") &&
                      strcmp(argv[1], "double_stop"))) {
        fprintf(stderr, "usage: %s case existing-file\n", argv[0]);
        return 1;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    case_name = argv[1];
    printf("case: %s\n", case_name);
    progress("initialization");
    if ((rc = pthread_create(&thread, NULL, watch, NULL))) {
        fprintf(stderr, "FAIL: %s: watchdog pthread_create returned %d\n", case_name, rc);
        return 1;
    }
    svr = (rtksvr_t*)calloc(1, sizeof(rtksvr_t));
    CHECK(svr != NULL, "calloc failed");
    if (!svr) return 1;
    CHECK(rtksvrinit(svr), "rtksvrinit failed");
    if (fails) return 1;
    opt.prcopt = prcopt_default;
    opt.solopt[0] = opt.solopt[1] = solopt_default;
    for (i = 0; i < 8; i++) {
        opt.strs[i] = STR_NONE;
        opt.paths[i] = "";
    }
    for (i = 0; i < 3; i++) {
        opt.formats[i] = STRFMT_RTCM3;
        opt.rcvopts[i] = "";
    }

    if (!strcmp(case_name, "start_state"))
        test_start_state(svr, &opt);
    else if (!strcmp(case_name, "stop_after_start"))
        test_stop_after_start(svr, &opt);
    else if (!strcmp(case_name, "start_failure_cleanup"))
        test_start_failure_cleanup(svr, &opt, argv[2]);
    else if (!strcmp(case_name, "stop_without_start"))
        test_stop_without_start(svr, &opt);
    else
        test_double_stop(svr, &opt);

    progress("free server");
    rtksvrfree(svr);
    pthread_mutex_destroy(&svr->lock);
    free(svr);
    pthread_mutex_lock(&watchdog.lock);
    watchdog.done = 1;
    pthread_mutex_unlock(&watchdog.lock);
    rc = pthread_join(thread, NULL);
    CHECK(rc == 0, "watchdog pthread_join failed");
    if (fails) {
        printf("%s: %d check(s) FAILED\n", case_name, fails);
        return 1;
    }
    printf("PASS: %s\n", case_name);
    return 0;
}
