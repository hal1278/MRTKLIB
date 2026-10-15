/*------------------------------------------------------------------------------
 * unit test : command-layer lifecycle events and adapter event boundaries
 * One process runs the cases in order because sysopts permits one instance.
 *----------------------------------------------------------------------------*/
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "mrtklib/mrtk_const.h"
#include "mrtklib/mrtk_context.h"
#include "mrtklib/mrtk_geoid.h"
#include "mrtklib/mrtk_sys.h"
#include "mrtklib/mrtk_trace.h"
#include "rcvcmd.h"

static int fails;
static rcvevent_t events[64];
static rcvcmd_solution_t solutions;
static rcvcmd_error_t errors;
static rcvcmd_io_t io;
static int start_command_notices, stop_command_notices;
static int notice_counts[RCVCMD_NOTICE_SERVER_STOPPED + 1];

#define CHECK(cond, msg)                          \
    do {                                          \
        if (!(cond)) {                            \
            fprintf(stderr, "FAIL: %s\n", (msg)); \
            fails++;                              \
        }                                         \
    } while (0)

#ifdef RCVCMD_TEST_THREADS
#include <errno.h>

static int fail_thread_create, collector_created, collector_joined, create_calls;
static pthread_t collector_thread;
int __real_pthread_create(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
int __real_pthread_join(pthread_t, void**);

int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*run)(void*), void* arg) {
    int result;
    create_calls++;
    if (fail_thread_create) return EAGAIN;
    result = __real_pthread_create(thread, attr, run, arg);
    if (!result && !collector_created) {
        collector_thread = *thread;
        collector_created = 1;
    }
    return result;
}

int __wrap_pthread_join(pthread_t thread, void** result) {
    int status = __real_pthread_join(thread, result);
    if (collector_created && pthread_equal(thread, collector_thread)) {
        CHECK(status == 0, "collector join succeeds");
        collector_joined++;
    }
    return status;
}

static int resource_case(const char* name) {
    rcvcmd_t* cmd;
    mrtk_ctx_t* ctx;
    int close_first = !strcmp(name, "close_without_initialize");
    int failure = !strcmp(name, "collector_create_failure");
    if (!close_first && !failure && strcmp(name, "destroy_without_initialize")) return 1;
    if (failure) {
        fail_thread_create = 1;
        ctx = mrtk_ctx_create();
        if (!ctx) return 1;
        g_mrtk_ctx = ctx;
        CHECK(rcvcmd_create(ctx) == NULL, "collector creation failure rejects command instance");
        CHECK(!collector_created && !collector_joined, "failed thread creation has no thread to join");
        CHECK(create_calls == 1, "sink registration creates no thread on failure");
        /* The freed ring must no longer receive messages (cap 0 would divide by zero). */
        trace(NULL, 1, "trace after a failed create\n");
        g_mrtk_ctx = NULL;
        mrtk_ctx_destroy(ctx);
        fail_thread_create = 0;
    }
    ctx = mrtk_ctx_create();
    if (!ctx) return 1;
    g_mrtk_ctx = ctx;
    cmd = rcvcmd_create(ctx);
    CHECK(cmd != NULL && collector_created, "collector starts at create, including after a failed create");
    CHECK(create_calls == 1 + failure, "only the collector creates a thread");
    if (!cmd) return 1;
    if (close_first) {
        rcvcmd_close(cmd);
        CHECK(collector_joined == 1, "close without initialize joins collector");
        rcvcmd_close(cmd);
        CHECK(collector_joined == 1, "repeated close joins only once");
    }
    g_mrtk_ctx = NULL;
    mrtk_ctx_destroy(ctx);
    rcvcmd_destroy(cmd);
    CHECK(collector_joined == 1, "destroy joins collector exactly once without initialize");
    printf("%s: %s (%d failures)\n", name, fails ? "FAILED" : "PASSED", fails);
    return fails ? 1 : 0;
}
#endif

static void on_notice(void* arg, const rcvcmd_notice_t* value) {
    rcvcmd_t* cmd = (rcvcmd_t*)arg;
    rcvcmd_snapshot_t snapshot;
    rcvcmd_result_t result;
    notice_counts[value->code]++;
    if (value->code == RCVCMD_NOTICE_START_COMMAND_ERROR) {
        start_command_notices++;
        CHECK(strcmp(value->text, "exit 3 # u13-startsecret") == 0 && WIFEXITED(value->status) &&
                  WEXITSTATUS(value->status) == 3 && value->status != 3,
              "start command notice retains command text and raw wait status");
    } else if (value->code == RCVCMD_NOTICE_STOP_COMMAND_ERROR) {
        stop_command_notices++;
        CHECK(strcmp(value->text, "kill -9 $$ # u13-stopsecret") == 0 && WIFSIGNALED(value->status) &&
                  WTERMSIG(value->status) == 9,
              "stop command notice retains command text and raw wait status");
    }
    /* Reentrant snapshot proves that notices run outside the publish mutex. */
    rcvcmd_snapshot(cmd, &snapshot);
    CHECK(snapshot.seq == rcvcmd_last_seq(cmd), "notice sees published boundary");
    result = rcvcmd_start(cmd, &io);
    CHECK(result.kind == (rcvcmd_shutdown_requested(cmd) ? RCVCMD_SHUTTING_DOWN : RCVCMD_BUSY) &&
              result.seq == snapshot.seq && rcvcmd_last_seq(cmd) == snapshot.seq,
          "refused request during notice publishes nothing");
}

static int overwrite(void* arg, const char* path) {
    (void)arg;
    (void)path;
    return 1;
}

static int read_all(rcvcmd_t* cmd) {
    rcvevent_read_t read;
    int i;
    CHECK(rcvcmd_read_events(cmd, 0, events, 64, &read) == 0, "read from zero");
    CHECK(!read.lost && !read.more, "all test events held and copied");
    CHECK(read.next == rcvcmd_last_seq(cmd), "read boundary equals last seq");
    for (i = 0; i < read.n; i++) CHECK(events[i].seq == (uint64_t)i + 1, "consecutive seq across kinds");
    return read.n;
}

static void status_at(int i, rcvcmd_state_t state, rcvcmd_stop_reason_t reason, rcvcmd_start_error_t error) {
    CHECK(events[i].kind == RCVEVENT_STATUS, "status kind");
    CHECK(events[i].data.status.state == (int)state, "status state");
    CHECK(events[i].data.status.stop_reason == (int)reason, "status stop reason");
    CHECK(events[i].data.status.last_error == (int)error, "status last error");
}

static void set_option(rcvcmd_t* cmd, const char* name, const char* value) {
    rcvcmd_option_t option;
    int found = rcvcmd_find_option(cmd, name, &option);
    CHECK(found, "option exists");
    if (found) CHECK(rcvcmd_set_option(cmd, &option, value) == RCVCMD_CONFIG_SUCCESS, "set option");
}

static void destroy_context(rcvcmd_t* cmd, mrtk_ctx_t* ctx) {
    rcvcmd_close(cmd);
    g_mrtk_ctx = NULL;
    mrtk_ctx_destroy(ctx);
    rcvcmd_destroy(cmd);
}

static void log_at(int i, int level, const char* text) {
    CHECK(events[i].kind == RCVEVENT_LOG && events[i].data.log.level == level &&
              events[i].data.log.origin == RCVEVENT_LOG_TRACE && !strcmp(events[i].data.log.text, text),
          "trace event has expected level, origin and text");
}

static atomic_int trace_done, trace_go;

static void* trace_worker(void* arg) {
    int i;
    (void)arg;
    while (!atomic_load(&trace_go)) sleepms(1);
    for (i = 0; i < 5000; i++) trace(NULL, 1, "worker %d\n", i);
    atomic_fetch_add(&trace_done, 1);
    return NULL;
}

static void sink_checks(void) {
    mrtk_ctx_t* ctx = mrtk_ctx_create();
    rcvcmd_t* cmd;
    pthread_t threads[4];
    rcvevent_read_t read;
    uint64_t last, cursor;
    char long_text[3001];
    int i, started = 0, toggles = 0;
    CHECK(ctx != NULL, "sink test context created");
    if (!ctx) return;
    g_mrtk_ctx = ctx;
    cmd = rcvcmd_create(ctx);
    CHECK(cmd != NULL, "sink test instance created");
    if (!cmd) {
        g_mrtk_ctx = NULL;
        mrtk_ctx_destroy(ctx);
        return;
    }
    CHECK(rcvcmd_log_level(cmd) == 1, "default log level is 1");
    trace(NULL, 1, "a\n");
    trace(NULL, 2, "hidden\n");
    CHECK(read_all(cmd) == 1, "level 1 produces exactly one event");
    log_at(0, 1, "a");
    CHECK(rcvcmd_set_log_level(cmd, 2) && rcvcmd_log_level(cmd) == 2, "select log level 2");
    trace(NULL, 2, "warning\n");
    trace(NULL, 3, "never\n");
    trace(NULL, 1, "CRLF\r\n");
    trace(NULL, 1, "\n");
    trace(NULL, 1, "\r\n");
    trace(NULL, 1, "part");
    trace(NULL, 1, "next");
    tracet(NULL, 2, "timed\n");
    memset(long_text, 'x', sizeof(long_text) - 1);
    long_text[sizeof(long_text) - 1] = '\0';
    trace(NULL, 1, "%s", long_text);
    CHECK(read_all(cmd) == 7, "calls stay separate, empty messages and level 3 are dropped");
    log_at(1, 2, "warning");
    log_at(2, 1, "CRLF");
    log_at(3, 1, "part");
    log_at(4, 1, "next");
    log_at(5, 2, "timed");
    long_text[RCVEVENT_TEXT - 1] = '\0';
    log_at(6, 1, long_text);
    CHECK(strlen(events[6].data.log.text) == 511, "3000-byte trace is cut to 511 bytes");
    for (i = 1; i <= 2; i++) {
        CHECK(rcvcmd_set_log_level(cmd, i), "select valid level before invalid requests");
        CHECK(!rcvcmd_set_log_level(cmd, 0) && !rcvcmd_set_log_level(cmd, 3) && !rcvcmd_set_log_level(cmd, -1) &&
                  rcvcmd_log_level(cmd) == i,
              "invalid levels leave current log level unchanged");
    }
    cursor = 0;
    CHECK(rcvcmd_read_errors(cmd, &cursor, &errors) > 0 && cursor == 7 &&
              !strncmp(errors.errbuf, "a\nwarning\nCRLF\npart\nnext\ntimed\n", 31) && strstr(errors.errbuf, long_text),
          "level 2 error reader includes trace entries");
    /* No server is running: only the four workers can advance the sequence. */
    last = cursor = rcvcmd_last_seq(cmd);
    atomic_store(&trace_done, 0);
    atomic_store(&trace_go, 0);
    for (i = 0; i < 4; i++) {
        int status = pthread_create(&threads[started], NULL, trace_worker, NULL);
        CHECK(status == 0, "trace worker starts");
        if (!status) started++;
    }
    atomic_store(&trace_go, 1);
    do {
        CHECK(rcvcmd_set_log_level(cmd, 1 + toggles % 2), "toggle log level concurrently");
        CHECK(rcvcmd_read_events(cmd, cursor, events, 64, &read) == 0, "read concurrent trace events");
        cursor = read.lost ? read.oldest - 1 : read.next;
        toggles++;
    } while (atomic_load(&trace_done) < started);
    for (i = 0; i < started; i++) CHECK(pthread_join(threads[i], NULL) == 0, "trace worker joins");
    CHECK(started == 4 && rcvcmd_last_seq(cmd) == last + 20000, "all 20000 calls publish despite ring wrap");
    CHECK(toggles > 0, "main thread toggles and reads during trace workload");
    destroy_context(cmd, ctx);
}

static void quiet_notice(void* arg, const rcvcmd_notice_t* value) {
    rcvcmd_t* cmd = (rcvcmd_t*)arg;
    rcvevent_read_t read;
    rcvevent_t ev;
    char expected[MRTK_TRACE_SINK_MSGLEN];
    uint64_t last = rcvcmd_last_seq(cmd);
    notice_counts[value->code]++;
    if (value->code == RCVCMD_NOTICE_SERVER_STOPPED) return;
    switch (value->code) {
        case RCVCMD_NOTICE_CORRECTION_ERROR:
            snprintf(expected, sizeof(expected), "start error: %s", value->text);
            break;
        case RCVCMD_NOTICE_NO_COMMAND_FILE:
            snprintf(expected, sizeof(expected), "no command file: %s", value->path);
            break;
        case RCVCMD_NOTICE_NO_ANTENNA:
            snprintf(expected, sizeof(expected), "no antenna %s in %s", value->text, value->path);
            break;
        case RCVCMD_NOTICE_ANTENNA_FILE_ERROR:
            snprintf(expected, sizeof(expected), "antenna file open error: %s", value->path);
            break;
        case RCVCMD_NOTICE_GEOID_FILE_ERROR:
            snprintf(expected, sizeof(expected), "geoid data open error: %s", value->path);
            break;
        case RCVCMD_NOTICE_SERVER_START_ERROR:
            strcpy(expected, "rtk server start error (str4 open error path=user:***@:2101)");
            break;
        case RCVCMD_NOTICE_START_COMMAND_ERROR:
            strcpy(expected, "misc-startcmd failed (exit code 3)");
            break;
        case RCVCMD_NOTICE_STOP_COMMAND_ERROR:
            strcpy(expected, "misc-stopcmd failed (signal 9)");
            break;
        default:
            CHECK(0, "unexpected notice in fixture");
            return;
    }
    CHECK(last && rcvcmd_read_events(cmd, last - 1, &ev, 1, &read) == 0 && read.n == 1,
          "notice publishes before adapter callback");
    expected[RCVEVENT_TEXT - 1] = '\0';
    CHECK(ev.kind == RCVEVENT_LOG && ev.data.log.level == 1 && ev.data.log.origin == RCVEVENT_LOG_TRACE &&
              !strcmp(ev.data.log.text, expected),
          "notice becomes one trace event with today's English text");
}

static int count_logs(rcvcmd_t* cmd, uint64_t after, const char* needle) {
    rcvevent_read_t read;
    int i, count = 0;
    do {
        CHECK(rcvcmd_read_events(cmd, after, events, 64, &read) == 0 && !read.lost, "read fixture event batch");
        for (i = 0; i < read.n; i++) {
            if (events[i].kind != RCVEVENT_LOG) continue;
            CHECK(!strstr(events[i].data.log.text, "u13-startsecret") &&
                      !strstr(events[i].data.log.text, "u13-stopsecret") &&
                      !strstr(events[i].data.log.text, "u13-pass"),
                  "no log event discloses commands or stream passwords");
            if (strstr(events[i].data.log.text, needle)) count++;
        }
        after = read.next;
    } while (read.more);
    return count;
}

static void notice_checks(char* config) {
    mrtk_ctx_t* ctx = mrtk_ctx_create();
    rcvcmd_t* cmd;
    rcvcmd_io_t output = {0};
    rcvcmd_result_t result;
    gtime_t rst = {0};
    uint64_t last;
    FILE* fp;
    char text[32768], long_path[701];
    size_t n;
    int level, before;
    CHECK(ctx != NULL, "notice test context created");
    if (!ctx) return;
    g_mrtk_ctx = ctx;
    cmd = rcvcmd_create(ctx);
    CHECK(cmd != NULL, "notice test instance created");
    if (!cmd) {
        g_mrtk_ctx = NULL;
        mrtk_ctx_destroy(ctx);
        return;
    }
    rcvcmd_initialize(cmd, config, "admin", "", 0, 0, rst);
    output.arg = cmd;
    output.notice = quiet_notice;
    output.confirm_overwrite = overwrite;
    set_option(cmd, "misc-startcmd", "");
    set_option(cmd, "misc-stopcmd", "");
    for (level = 1; level <= 2; level++) {
        CHECK(rcvcmd_set_log_level(cmd, level), "set correction test log level");
        set_option(cmd, "pos1-correction", "bds-b2b");
        last = rcvcmd_last_seq(cmd);
        result = rcvcmd_start(cmd, &output);
        CHECK(result.kind == RCVCMD_START_FAILED && count_logs(cmd, last, "start error:") == 1,
              "correction failure logs exactly once at either log level");
    }
    /* Credentials exist only in network stream paths; file paths are not secrets and the core
     * traces them at level 1. An NTRIP caster output without a mountpoint fails before listening. */
    set_option(cmd, "pos1-correction", "none");
    set_option(cmd, "outstr1-type", "ntripcas");
    set_option(cmd, "outstr1-path", "user:u13-pass@:2101");
    last = rcvcmd_last_seq(cmd);
    CHECK(rcvcmd_start(cmd, &output).kind == RCVCMD_START_FAILED, "stream password fixture fails at level 2");
    CHECK(count_logs(cmd, last, "rtk server start error") == 1, "one masked server failure at level 2");
    set_option(cmd, "outstr1-type", "off");
    set_option(cmd, "outstr1-path", "");
    CHECK(rcvcmd_set_log_level(cmd, 1), "notice fixtures use level 1 to exclude core warnings");
    fp = fopen("empty.atx", "w");
    CHECK(fp != NULL, "create empty antenna fixture");
    if (fp) fclose(fp);
    set_option(cmd, "file-cmdfile1", "missing.cmd");
    set_option(cmd, "file-rcvantfile", "empty.atx");
    set_option(cmd, "file-satantfile", "missing-satellite.atx");
    set_option(cmd, "ant1-anttype", "U13-MISSING-ANTENNA");
    set_option(cmd, "out-geoid", "gsi2000");
    set_option(cmd, "file-geoidfile", "missing-geoid.dat");
    last = rcvcmd_last_seq(cmd);
    CHECK(rcvcmd_start(cmd, &output).kind == RCVCMD_SUCCESS, "notice fixture server starts");
    CHECK(count_logs(cmd, last, "no command file: missing.cmd") == 2, "each failed command-file read logs once");
    CHECK(count_logs(cmd, last, "no antenna U13-MISSING-ANTENNA in empty.atx") == 1, "missing antenna logs once");
    CHECK(count_logs(cmd, last, "antenna file open error: missing-satellite.atx") == 1,
          "satellite antenna error logs once");
    CHECK(count_logs(cmd, last, "geoid data open error: missing-geoid.dat") == 1, "geoid notice logs once");
    rcvcmd_stop(cmd, &output);
    set_option(cmd, "file-cmdfile1", "");
    set_option(cmd, "file-rcvantfile", "missing-receiver.atx");
    set_option(cmd, "file-satantfile", "empty.atx");
    set_option(cmd, "out-geoid", "internal");
    set_option(cmd, "file-geoidfile", "");
    last = rcvcmd_last_seq(cmd);
    CHECK(rcvcmd_start(cmd, &output).kind == RCVCMD_SUCCESS, "receiver antenna failure is nonfatal");
    CHECK(count_logs(cmd, last, "antenna file open error: missing-receiver.atx") == 1,
          "receiver antenna error logs once");
    rcvcmd_stop(cmd, &output);
    set_option(cmd, "file-rcvantfile", "");
    set_option(cmd, "file-satantfile", "");
    set_option(cmd, "ant1-anttype", "");
    CHECK(unlink("empty.atx") == 0, "remove antenna fixture");
    /* The file keeps the notice beyond the ring's 511-byte limit. */
    memset(long_path, 'p', sizeof(long_path) - 1);
    long_path[sizeof(long_path) - 1] = '\0';
    set_option(cmd, "file-cmdfile1", long_path);
    traceopen(ctx, "long-notice.trace");
    tracelevel(ctx, 2);
    last = rcvcmd_last_seq(cmd);
    CHECK(rcvcmd_start(cmd, &output).kind == RCVCMD_SUCCESS, "long notice fixture starts");
    rcvcmd_stop(cmd, &output);
    traceclose(ctx);
    /* start reads the command file twice (start and periodic commands), stop once */
    CHECK(count_logs(cmd, last, "no command file:") == 3, "each long notice produces one ring entry");
    fp = fopen("long-notice.trace", "r");
    CHECK(fp != NULL, "read long notice trace");
    if (fp) {
        n = fread(text, 1, sizeof(text) - 1, fp);
        text[n] = '\0';
        CHECK(strstr(text, long_path) != NULL, "trace file retains the full notice beyond 511 bytes");
        fclose(fp);
    }
    CHECK(unlink("long-notice.trace") == 0, "remove long notice trace");
    set_option(cmd, "file-cmdfile1", "");
    CHECK(rcvcmd_set_log_level(cmd, 2), "enable warning trace events for command tests");
    set_option(cmd, "misc-startcmd", "exit 3 # u13-startsecret");
    set_option(cmd, "misc-stopcmd", "kill -9 $$ # u13-stopsecret");
    for (level = 2; level <= 3; level++) {
        traceopen(ctx, "commands.trace");
        CHECK(ctx->trace_fp != NULL, "open temporary command trace file");
        tracelevel(ctx, level);
        last = rcvcmd_last_seq(cmd);
        CHECK(rcvcmd_start(cmd, &output).kind == RCVCMD_SUCCESS, "command fixture starts");
        CHECK(rcvcmd_stop(cmd, &output).kind == RCVCMD_SUCCESS, "command fixture stops");
        traceclose(ctx);
        CHECK(count_logs(cmd, last, "misc-startcmd") == 1 && count_logs(cmd, last, "misc-stopcmd") == 1,
              "failed start and stop commands each publish exactly one masked event at level 2");
        fp = fopen("commands.trace", "r");
        CHECK(fp != NULL, "read command trace file");
        if (fp) {
            n = fread(text, 1, sizeof(text) - 1, fp);
            text[n] = '\0';
            CHECK(feof(fp), "trace file fits test buffer");
            fclose(fp);
            CHECK(strstr(text, "misc-startcmd failed (exit code 3)") && strstr(text, "misc-stopcmd failed (signal 9)"),
                  "trace file contains masked level-1 failures");
            CHECK((strstr(text, "execcmd: cmd=exit 3 # u13-startsecret") != NULL) == (level == 3) &&
                      (strstr(text, "execcmd: cmd=kill -9 $$ # u13-stopsecret") != NULL) == (level == 3),
                  "execcmd records full commands only at file level 3");
            if (level == 2)
                CHECK(!strstr(text, "u13-startsecret") && !strstr(text, "u13-stopsecret"),
                      "trace file level 2 has no command marker");
        }
        CHECK(unlink("commands.trace") == 0, "remove command trace file");
    }
    set_option(cmd, "misc-startcmd", "");
    set_option(cmd, "misc-stopcmd", "");
    last = rcvcmd_last_seq(cmd);
    before = notice_counts[RCVCMD_NOTICE_SERVER_STOPPED];
    traceopen(ctx, "stopped.trace");
    tracelevel(ctx, 2);
    rcvcmd_start(cmd, &output);
    rcvcmd_stop(cmd, &output);
    traceclose(ctx);
    CHECK(count_logs(cmd, last, "") == 0 && notice_counts[RCVCMD_NOTICE_SERVER_STOPPED] == before + 1,
          "server stopped adapter notice produces no log");
    fp = fopen("stopped.trace", "r");
    CHECK(fp != NULL, "read stopped trace file");
    if (fp) {
        CHECK(fgetc(fp) == EOF, "server stopped produces no trace");
        fclose(fp);
    }
    CHECK(unlink("stopped.trace") == 0, "remove stopped trace file");
    for (level = 0; level <= RCVCMD_NOTICE_SERVER_STOPPED; level++)
        CHECK(notice_counts[level] > 0, "every notice code exercised");
    destroy_context(cmd, ctx);
}

static void replay_notice(void* arg, const rcvcmd_notice_t* value) {
    (void)arg;
    CHECK(value->code == RCVCMD_NOTICE_SERVER_STOPPED, "replay has no error notices");
}

static void stdin_notice(void* arg, const rcvcmd_notice_t* value) {
    rcvcmd_t* cmd = (rcvcmd_t*)arg;
    rcvcmd_snapshot_t snapshot;
    CHECK(value->code == RCVCMD_NOTICE_SERVER_STOPPED ||
              (value->code == RCVCMD_NOTICE_SERVER_START_ERROR &&
               !strcmp(value->text, "stdin is reserved by --exit-on-stdin-eof")),
          "stdin reservation uses the existing server failure notice");
    rcvcmd_snapshot(cmd, &snapshot);
    rcvcmd_reserve_stdin(cmd);
}

static void stdin_checks(char* config) {
    static const char* paths[] = {"", "::T", "::x1"};
    mrtk_ctx_t* ctx = mrtk_ctx_create();
    rcvcmd_t* cmd;
    rcvcmd_io_t output = {0};
    rcvcmd_lifecycle_t lifecycle;
    rcvcmd_result_t result;
    rcvcmd_snapshot_t snapshot;
    gtime_t rst = {0};
    char option[32];
    FILE* fp;
    size_t i;
    int slot;
    CHECK(ctx != NULL, "stdin test context created");
    if (!ctx) return;
    g_mrtk_ctx = ctx;
    cmd = rcvcmd_create(ctx);
    CHECK(cmd != NULL, "stdin test command instance created");
    if (!cmd) {
        g_mrtk_ctx = NULL;
        mrtk_ctx_destroy(ctx);
        return;
    }
    rcvcmd_initialize(cmd, config, "admin", "", 0, 0, rst);
    output.arg = cmd;
    output.notice = stdin_notice;
    output.confirm_overwrite = overwrite;
    rcvcmd_reserve_stdin(cmd);
    rcvcmd_reserve_stdin(cmd);
    set_option(cmd, "misc-startcmd", "touch stdin-started");
    for (slot = 1; slot <= 3; slot++) {
        snprintf(option, sizeof(option), "inpstr%d-type", slot);
        set_option(cmd, option, "file");
        for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
            snprintf(option, sizeof(option), "inpstr%d-path", slot);
            set_option(cmd, option, paths[i]);
            result = rcvcmd_start(cmd, &output);
            CHECK(result.kind == RCVCMD_START_FAILED && result.state == RCVCMD_STOPPED,
                  "reserved stdin input fails start in each input slot");
            rcvcmd_get_lifecycle(cmd, &lifecycle);
            CHECK(
                lifecycle.last_error == RCVCMD_START_ERROR_STDIN_RESERVED && lifecycle.stop_reason == RCVCMD_STOP_ERROR,
                "stdin reservation has a structured start failure");
            rcvcmd_snapshot(cmd, &snapshot);
            CHECK(snapshot.lifecycle.last_error == RCVCMD_START_ERROR_STDIN_RESERVED,
                  "stdin reservation error is visible in sync/getStatus snapshot");
            CHECK(access("stdin-started", F_OK) != 0, "stdin refusal happens before the start command");
        }
        snprintf(option, sizeof(option), "inpstr%d-type", slot);
        set_option(cmd, option, "off");
    }
    set_option(cmd, "misc-startcmd", "");
    fp = fopen("stdin-input.bin", "w");
    CHECK(fp != NULL, "create named file input fixture");
    if (fp) fclose(fp);
    set_option(cmd, "inpstr1-type", "file");
    set_option(cmd, "inpstr1-path", "stdin-input.bin");
    CHECK(rcvcmd_start(cmd, &output).kind == RCVCMD_SUCCESS, "stdin reservation permits named file inputs");
    rcvcmd_get_lifecycle(cmd, &lifecycle);
    CHECK(lifecycle.last_error == RCVCMD_START_ERROR_NONE, "successful start clears stdin reservation error");
    rcvcmd_shutdown(cmd, &output);
    destroy_context(cmd, ctx);
    CHECK(unlink("stdin-input.bin") == 0, "remove named file input fixture");
}

static void verify_error_reader(rcvcmd_t* cmd, uint64_t after, int level) {
    uint64_t cursor = after, last = rcvcmd_last_seq(cmd);
    int reads = 0;
    do {
        char expected[MAXERRMSG + 1] = "";
        size_t used = 0;
        uint64_t scan = cursor;
        rcvevent_read_t read;
        int i;
        rcvcmd_read_errors(cmd, &cursor, &errors);
        CHECK(cursor > scan || cursor == last, "telnet error cursor makes progress");
        while (scan < cursor) {
            CHECK(rcvcmd_read_events(cmd, scan, events, 64, &read) == 0 && !read.lost && read.n > 0,
                  "read events covered by telnet error cursor");
            if (!read.n) break;
            for (i = 0; i < read.n && events[i].seq <= cursor; i++) {
                rcvevent_t* ev = &events[i];
                scan = ev->seq;
                if (ev->kind == RCVEVENT_LOG && (level == 2 || ev->data.log.origin == RCVEVENT_LOG_ERRBUF)) {
                    size_t n = strlen(ev->data.log.text);
                    CHECK(used + n + 1 <= MAXERRMSG, "selected logs fit telnet read buffer");
                    if (used + n + 1 > MAXERRMSG) return;
                    memcpy(expected + used, ev->data.log.text, n);
                    used += n;
                    expected[used++] = '\n';
                    expected[used] = '\0';
                }
            }
        }
        CHECK(errors.n == (int)used && !strcmp(errors.errbuf, expected),
              "telnet reader returns exactly the origins selected by current level, in order");
    } while (cursor < last && ++reads < 100);
    CHECK(cursor == last, "telnet error reader reaches the event boundary");
}

static void errbuf_checks(rcvcmd_t* cmd, const char* path) {
    rcvcmd_io_t output = {0};
    char stream[1024], first_errors[64][RCVEVENT_TEXT];
    int level, i, count, errbuf, traces, missing, matched, first_count = 0;
    uint64_t after, cursor;
    uint32_t started;
    rcvevent_read_t read;
    snprintf(stream, sizeof(stream), "%s::T::x10", path);
    CHECK(access(path, R_OK) == 0, "MALIB fixture is readable");
    set_option(cmd, "inpstr1-type", "file");
    set_option(cmd, "inpstr1-path", stream);
    set_option(cmd, "inpstr1-format", "sbf");
    set_option(cmd, "pos1-posmode", "kinematic");
    set_option(cmd, "pos1-correction", "none");
    set_option(cmd, "pos1-sateph", "brdc");
    set_option(cmd, "out-outsingle", "on");
    output.arg = cmd;
    output.notice = replay_notice;
    output.confirm_overwrite = overwrite;
    for (level = 1; level <= 2; level++) {
        CHECK(rcvcmd_set_log_level(cmd, level), "select replay log level");
        after = cursor = rcvcmd_last_seq(cmd);
        CHECK(rcvcmd_start(cmd, &output).kind == RCVCMD_SUCCESS, "MALIB replay starts");
        started = tickget();
        missing = 0;
        do {
            sleepms(50);
            CHECK(rcvcmd_read_events(cmd, cursor, events, 64, &read) == 0 && !read.lost, "poll real RTK errors");
            for (i = 0; i < read.n; i++) {
                if (events[i].kind == RCVEVENT_LOG &&
                    strstr(events[i].data.log.text, "no base station observation data for rtk")) {
                    if (level == 1) {
                        missing++;
                    } else {
                        int j;
                        /* Restart retains navigation: await the first run's interval. */
                        for (j = 0; j < first_count; j++) {
                            if (!strcmp(first_errors[j], events[i].data.log.text)) {
                                missing++;
                                break;
                            }
                        }
                    }
                }
            }
            cursor = read.next;
        } while (missing < 2 && tickget() - started < 15000);
        CHECK(missing >= 2, "real missing-base errors arrive within 15 seconds");
        CHECK(rcvcmd_stop(cmd, &output).kind == RCVCMD_SUCCESS, "MALIB replay stops and drains errors");
        cursor = after;
        count = errbuf = traces = matched = 0;
        do {
            CHECK(rcvcmd_read_events(cmd, cursor, events, 64, &read) == 0 && !read.lost, "read drained replay logs");
            for (i = 0; i < read.n; i++) {
                rcvevent_t* ev = &events[i];
                if (ev->kind != RCVEVENT_LOG) continue;
                count++;
                if (ev->data.log.origin == RCVEVENT_LOG_ERRBUF) {
                    errbuf++;
                    CHECK(ev->data.log.level == 1, "errbuf is published at level 1");
                    if (first_count < 64 && strstr(ev->data.log.text, "no base station observation data for rtk"))
                        strcpy(first_errors[first_count++], ev->data.log.text);
                } else if (ev->data.log.level == 2 &&
                           strstr(ev->data.log.text, "no base station observation data for rtk")) {
                    int j;
                    traces++;
                    for (j = 0; j < first_count; j++) {
                        if (!strcmp(first_errors[j], ev->data.log.text)) {
                            matched++;
                            break;
                        }
                    }
                }
            }
            cursor = read.next;
        } while (read.more);
        if (level == 1) {
            CHECK(errbuf >= 2 && count == errbuf, "level 1 publishes real errbuf lines only");
            CHECK(first_count > 0, "retain real error text to probe origin filtering");
            if (first_count) trace(NULL, 1, "%s\n", first_errors[0]);
        } else {
            CHECK(errbuf == 0 && traces >= 2 && matched >= 2,
                  "level 2 has no new errbuf events and trace carries the same real error texts");
        }
        trace(NULL, 1, "reader marker\n");
        verify_error_reader(cmd, after, level);
        if (level == 2) verify_error_reader(cmd, 0, level);
    }
}

int main(int argc, char** argv) {
    char directory[] = "utest_rcvcmd_events_XXXXXX";
    char config[1024] = "events.conf";
    char* original = getcwd(NULL, 0);
    FILE* fp;
    rcvcmd_t* cmd;
    mrtk_ctx_t* ctx;
    rcvcmd_result_t result;
    rcvcmd_snapshot_t snapshot;
    rcvevent_read_t read;
    uint64_t cursor, other, last;
    gtime_t rst = {0};
    double geoid_pos[2] = {30.0 * D2R, 0.0}, embedded_height;
    int n;

#ifdef RCVCMD_TEST_THREADS
    if (argc == 2) {
        free(original);
        return resource_case(argv[1]);
    }
#endif
    if (argc != 1 && (argc != 3 || strcmp(argv[1], "errbuf"))) return 1;
    if (!original || !mkdtemp(directory) || chdir(directory)) return 1;
    fp = fopen(config, "w");
    if (!fp) return 1;
    fputs(
        "inpstr1-type = off\ninpstr2-type = off\ninpstr3-type = off\n"
        "outstr1-type = off\noutstr2-type = off\n"
        "logstr1-type = off\nlogstr2-type = off\nlogstr3-type = off\n"
        "pos1-posmode = single\n",
        fp);
    if (fclose(fp)) return 1;
    ctx = mrtk_ctx_create();
    if (!ctx) return 1;
    g_mrtk_ctx = ctx;
    cmd = rcvcmd_create(ctx);
    if (!cmd) return 1;
    CHECK(rcvcmd_create(ctx) == NULL, "second instance refused");
    CHECK(rcvcmd_last_seq(cmd) == 0, "empty boundary at create");
    rcvcmd_initialize(cmd, config, "admin", "", 0, 0, rst);
    if (argc == 3) {
        errbuf_checks(cmd, argv[2]);
        destroy_context(cmd, ctx);
        goto cleanup_files;
    }
    io.arg = cmd;
    io.notice = on_notice;
    io.confirm_overwrite = overwrite;

    result = rcvcmd_start(cmd, &io);
    CHECK(result.kind == RCVCMD_SUCCESS && result.state == RCVCMD_RUNNING && result.seq == 1,
          "start result carries claim seq");
    CHECK(read_all(cmd) == 2, "start publishes starting and running");
    status_at(0, RCVCMD_STARTING, RCVCMD_STOP_NONE, RCVCMD_START_ERROR_NONE);
    status_at(1, RCVCMD_RUNNING, RCVCMD_STOP_NONE, RCVCMD_START_ERROR_NONE);
    rcvcmd_snapshot(cmd, &snapshot);
    CHECK(snapshot.seq == 2 && snapshot.lifecycle.state == RCVCMD_RUNNING && snapshot.mode == PMODE_SINGLE &&
              !snapshot.has_solution,
          "sync captures running mode with no solution");
    result = rcvcmd_start(cmd, &io);
    CHECK(result.kind == RCVCMD_ALREADY_RUNNING && result.seq == 2, "already running publishes nothing");
    sleepms(1600);
    CHECK(read_all(cmd) == 2, "time-less NONE solutions are skipped after collection");
    cursor = other = 0;
    CHECK(rcvcmd_read_solutions(cmd, &cursor, &solutions) == 0 && cursor == 2, "solution cursor advances over status");
    CHECK(rcvcmd_read_solutions(cmd, &other, &solutions) == 0 && other == cursor, "independent solution cursor");
    cursor = 0;
    CHECK(rcvcmd_read_errors(cmd, &cursor, &errors) == 0 && cursor == 2 && errors.errbuf[0] == '\0',
          "error cursor advances over status");

    result = rcvcmd_restart(cmd, &io);
    CHECK(result.kind == RCVCMD_SUCCESS && result.seq == 3, "restart claim seq");
    CHECK(read_all(cmd) == 6, "restart publishes four states");
    status_at(2, RCVCMD_STOPPING, RCVCMD_STOP_REQUEST, RCVCMD_START_ERROR_NONE);
    status_at(3, RCVCMD_STOPPED, RCVCMD_STOP_REQUEST, RCVCMD_START_ERROR_NONE);
    status_at(4, RCVCMD_STARTING, RCVCMD_STOP_REQUEST, RCVCMD_START_ERROR_NONE);
    status_at(5, RCVCMD_RUNNING, RCVCMD_STOP_REQUEST, RCVCMD_START_ERROR_NONE);
    result = rcvcmd_stop(cmd, &io);
    CHECK(result.kind == RCVCMD_SUCCESS && result.seq == 7, "stop claim seq");
    CHECK(read_all(cmd) == 8, "stop publishes stopping and stopped");
    status_at(6, RCVCMD_STOPPING, RCVCMD_STOP_REQUEST, RCVCMD_START_ERROR_NONE);
    status_at(7, RCVCMD_STOPPED, RCVCMD_STOP_REQUEST, RCVCMD_START_ERROR_NONE);
    result = rcvcmd_stop(cmd, &io);
    CHECK(result.kind == RCVCMD_SUCCESS && result.seq == 8 && read_all(cmd) == 8, "second stop publishes nothing");

    set_option(cmd, "pos1-correction", "bds-b2b");
    result = rcvcmd_start(cmd, &io);
    CHECK(result.kind == RCVCMD_START_FAILED && result.seq == 9, "failed start returns claim seq");
    CHECK(read_all(cmd) == 11, "failed start publishes two states and a layer log");
    status_at(8, RCVCMD_STARTING, RCVCMD_STOP_REQUEST, RCVCMD_START_ERROR_NONE);
    CHECK(events[9].kind == RCVEVENT_LOG && events[9].data.log.level == 1 &&
              events[9].data.log.origin == RCVEVENT_LOG_TRACE &&
              strncmp(events[9].data.log.text, "start error: ", 13) == 0,
          "correction notice becomes a layer log");
    status_at(10, RCVCMD_STOPPED, RCVCMD_STOP_ERROR, RCVCMD_START_ERROR_CORRECTION);
    cursor = 8;
    CHECK(rcvcmd_read_errors(cmd, &cursor, &errors) == 0 && cursor == 11, "telnet errors exclude layer logs");
    rcvcmd_snapshot(cmd, &snapshot);
    CHECK(snapshot.seq == 11 && snapshot.lifecycle.state == RCVCMD_STOPPED && !snapshot.has_solution,
          "stopped snapshot has no solution");
    CHECK(rcvcmd_read_events(cmd, 12, events, 64, &read) == -1, "future event cursor rejected");

    /* Stream-open detail must mask passwords even with an @ inside the password. */
    set_option(cmd, "pos1-correction", "none");
    set_option(cmd, "inpstr1-type", "file");
    set_option(cmd, "inpstr1-path", "user:sensitive@part@missing/observations");
    embedded_height = geoidh(geoid_pos);
    CHECK(embedded_height != 0.0, "embedded geoid probe is nonzero");
    fp = fopen("empty-geoid.dat", "w");
    if (!fp) return 1;
    if (fclose(fp)) return 1;
    /* GSI rejects this position before reading the empty file. */
    CHECK(opengeoid(GEOID_GSI2000_M15, "empty-geoid.dat") && geoidh(geoid_pos) == 0.0,
          "empty GSI file gives a safe, distinct model probe");
    closegeoid();
    set_option(cmd, "out-geoid", "gsi2000");
    set_option(cmd, "file-geoidfile", "empty-geoid.dat");
    result = rcvcmd_start(cmd, &io);
    CHECK(result.kind == RCVCMD_START_FAILED && result.seq == 12, "stream failure claim seq");
    CHECK(geoidh(geoid_pos) == embedded_height, "failed server start restores embedded geoid");
    CHECK(read_all(cmd) == 15, "stream failure events");
    CHECK(events[13].kind == RCVEVENT_LOG &&
              strcmp(events[13].data.log.text,
                     "rtk server start error (str1 open error path=user:***@missing/observations)") == 0,
          "password masked using last @ and first colon");
    status_at(14, RCVCMD_STOPPED, RCVCMD_STOP_ERROR, RCVCMD_START_ERROR_SERVER);
    set_option(cmd, "out-geoid", "internal");
    set_option(cmd, "file-geoidfile", "");
    CHECK(unlink("empty-geoid.dat") == 0, "temporary geoid probe cleanup");
    set_option(cmd, "inpstr1-type", "off");
    set_option(cmd, "misc-startcmd", "exit 3 # u13-startsecret");
    set_option(cmd, "misc-stopcmd", "kill -9 $$ # u13-stopsecret");
    result = rcvcmd_start(cmd, &io);
    CHECK(result.kind == RCVCMD_SUCCESS && result.state == RCVCMD_RUNNING && result.seq == 16,
          "recover after start failure despite failed start command");
    n = read_all(cmd);
    CHECK(n == 18, "recovery events include failed start command log");
    status_at(15, RCVCMD_STARTING, RCVCMD_STOP_ERROR, RCVCMD_START_ERROR_SERVER);
    CHECK(events[16].kind == RCVEVENT_LOG && events[16].data.log.level == 1 &&
              events[16].data.log.origin == RCVEVENT_LOG_TRACE &&
              strcmp(events[16].data.log.text, "misc-startcmd failed (exit code 3)") == 0 &&
              strstr(events[16].data.log.text, "u13-startsecret") == NULL && start_command_notices == 1,
          "start command log names option and decoded exit code without command text");
    status_at(17, RCVCMD_RUNNING, RCVCMD_STOP_ERROR, RCVCMD_START_ERROR_NONE);
    result = rcvcmd_stop(cmd, &io);
    CHECK(result.kind == RCVCMD_SUCCESS && result.state == RCVCMD_STOPPED && result.seq == 19,
          "stop succeeds despite signaled stop command");
    CHECK(read_all(cmd) == 21, "stop events include signaled stop command log");
    status_at(18, RCVCMD_STOPPING, RCVCMD_STOP_REQUEST, RCVCMD_START_ERROR_NONE);
    CHECK(events[19].kind == RCVEVENT_LOG && events[19].data.log.level == 1 &&
              events[19].data.log.origin == RCVEVENT_LOG_TRACE &&
              strcmp(events[19].data.log.text, "misc-stopcmd failed (signal 9)") == 0 && stop_command_notices == 1,
          "stop command log names option and decoded signal without command text");
    status_at(20, RCVCMD_STOPPED, RCVCMD_STOP_REQUEST, RCVCMD_START_ERROR_NONE);
    set_option(cmd, "misc-startcmd", "");
    set_option(cmd, "misc-stopcmd", "");
    result = rcvcmd_start(cmd, &io);
    CHECK(result.kind == RCVCMD_SUCCESS && result.seq == 22 && read_all(cmd) == 23,
          "start again with external commands cleared");
    rcvcmd_request_shutdown(cmd);
    last = rcvcmd_last_seq(cmd);
    result = rcvcmd_restart(cmd, &io);
    CHECK(result.kind == RCVCMD_SHUTTING_DOWN && result.seq == last && rcvcmd_last_seq(cmd) == last,
          "shutdown refusal publishes nothing");
    rcvcmd_shutdown(cmd, &io);
    CHECK(read_all(cmd) == 25, "shutdown uses stop events");
    status_at(23, RCVCMD_STOPPING, RCVCMD_STOP_REQUEST, RCVCMD_START_ERROR_NONE);
    status_at(24, RCVCMD_STOPPED, RCVCMD_STOP_REQUEST, RCVCMD_START_ERROR_NONE);
    rcvcmd_close(cmd);
    rcvcmd_close(cmd);
    g_mrtk_ctx = NULL;
    mrtk_ctx_destroy(ctx);
    rcvcmd_destroy(cmd);
#ifdef RCVCMD_TEST_THREADS
    CHECK(collector_joined == 1, "initialized collector joins exactly once");
#endif
    sink_checks();
    notice_checks(config);
    stdin_checks(config);
cleanup_files:
    CHECK(unlink(config) == 0 && chdir(original) == 0 && rmdir(directory) == 0, "temporary config cleanup");
    free(original);
    printf("%s (%d failures)\n", fails ? "FAILED" : "PASSED", fails);
    return fails ? 1 : 0;
}
