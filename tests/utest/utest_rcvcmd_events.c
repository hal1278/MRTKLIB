/*------------------------------------------------------------------------------
 * unit test : command-layer lifecycle events and adapter event boundaries
 * One process runs the cases in order because sysopts permits one instance.
 *----------------------------------------------------------------------------*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "mrtklib/mrtk_const.h"
#include "mrtklib/mrtk_geoid.h"
#include "mrtklib/mrtk_sys.h"
#include "rcvcmd.h"

static int fails;
static rcvevent_t events[64];
static rcvcmd_solution_t solutions;
static rcvcmd_error_t errors;
static rcvcmd_io_t io;
static int start_command_notices, stop_command_notices;

#define CHECK(cond, msg)                          \
    do {                                          \
        if (!(cond)) {                            \
            fprintf(stderr, "FAIL: %s\n", (msg)); \
            fails++;                              \
        }                                         \
    } while (0)

#ifdef RCVCMD_TEST_THREADS
#include <errno.h>
#include <pthread.h>

static int fail_thread_create, collector_created, collector_joined;
static pthread_t collector_thread;
int __real_pthread_create(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
int __real_pthread_join(pthread_t, void**);

int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*run)(void*), void* arg) {
    int result;
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
    int close_first = !strcmp(name, "close_without_initialize");
    int failure = !strcmp(name, "collector_create_failure");
    if (!close_first && !failure && strcmp(name, "destroy_without_initialize")) return 1;
    if (failure) {
        fail_thread_create = 1;
        CHECK(rcvcmd_create() == NULL, "collector creation failure rejects command instance");
        CHECK(!collector_created && !collector_joined, "failed thread creation has no thread to join");
        fail_thread_create = 0;
    }
    cmd = rcvcmd_create();
    CHECK(cmd != NULL && collector_created, "collector starts at create, including after a failed create");
    if (!cmd) return 1;
    if (close_first) {
        rcvcmd_close(cmd);
        CHECK(collector_joined == 1, "close without initialize joins collector");
        rcvcmd_close(cmd);
        CHECK(collector_joined == 1, "repeated close joins only once");
    }
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
    if (value->code == RCVCMD_NOTICE_START_COMMAND_ERROR) {
        start_command_notices++;
        CHECK(strcmp(value->text, "exit 3 # u11-marker") == 0 && WIFEXITED(value->status) &&
                  WEXITSTATUS(value->status) == 3 && value->status != 3,
              "start command notice retains command text and raw wait status");
    } else if (value->code == RCVCMD_NOTICE_STOP_COMMAND_ERROR) {
        stop_command_notices++;
        CHECK(strcmp(value->text, "kill -9 $$") == 0 && WIFSIGNALED(value->status) && WTERMSIG(value->status) == 9,
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

int main(int argc, char** argv) {
    char directory[] = "utest_rcvcmd_events_XXXXXX";
    char config[1024] = "events.conf";
    char* original = getcwd(NULL, 0);
    FILE* fp;
    rcvcmd_t* cmd;
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
    (void)argv;
    if (argc != 1) return 1;
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
    cmd = rcvcmd_create();
    if (!cmd) return 1;
    CHECK(rcvcmd_create() == NULL, "second instance refused");
    CHECK(rcvcmd_last_seq(cmd) == 0, "empty boundary at create");
    rcvcmd_initialize(cmd, config, "admin", "", 0, 0, rst);
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
              events[9].data.log.origin == RCVEVENT_LOG_LAYER &&
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
    CHECK(read_all(cmd) == 14, "stream failure events");
    CHECK(events[12].kind == RCVEVENT_LOG &&
              strcmp(events[12].data.log.text,
                     "rtk server start error (str1 open error path=user:***@missing/observations)") == 0,
          "password masked using last @ and first colon");
    status_at(13, RCVCMD_STOPPED, RCVCMD_STOP_ERROR, RCVCMD_START_ERROR_SERVER);
    set_option(cmd, "out-geoid", "internal");
    set_option(cmd, "file-geoidfile", "");
    CHECK(unlink("empty-geoid.dat") == 0, "temporary geoid probe cleanup");
    set_option(cmd, "inpstr1-type", "off");
    set_option(cmd, "misc-startcmd", "exit 3 # u11-marker");
    set_option(cmd, "misc-stopcmd", "kill -9 $$");
    result = rcvcmd_start(cmd, &io);
    CHECK(result.kind == RCVCMD_SUCCESS && result.state == RCVCMD_RUNNING && result.seq == 15,
          "recover after start failure despite failed start command");
    n = read_all(cmd);
    CHECK(n == 17, "recovery events include failed start command log");
    status_at(14, RCVCMD_STARTING, RCVCMD_STOP_ERROR, RCVCMD_START_ERROR_SERVER);
    CHECK(events[15].kind == RCVEVENT_LOG && events[15].data.log.level == 1 &&
              events[15].data.log.origin == RCVEVENT_LOG_LAYER &&
              strcmp(events[15].data.log.text, "misc-startcmd failed (exit code 3)") == 0 &&
              strstr(events[15].data.log.text, "u11-marker") == NULL && start_command_notices == 1,
          "start command log names option and decoded exit code without command text");
    status_at(16, RCVCMD_RUNNING, RCVCMD_STOP_ERROR, RCVCMD_START_ERROR_NONE);
    result = rcvcmd_stop(cmd, &io);
    CHECK(result.kind == RCVCMD_SUCCESS && result.state == RCVCMD_STOPPED && result.seq == 18,
          "stop succeeds despite signaled stop command");
    CHECK(read_all(cmd) == 20, "stop events include signaled stop command log");
    status_at(17, RCVCMD_STOPPING, RCVCMD_STOP_REQUEST, RCVCMD_START_ERROR_NONE);
    CHECK(events[18].kind == RCVEVENT_LOG && events[18].data.log.level == 1 &&
              events[18].data.log.origin == RCVEVENT_LOG_LAYER &&
              strcmp(events[18].data.log.text, "misc-stopcmd failed (signal 9)") == 0 && stop_command_notices == 1,
          "stop command log names option and decoded signal without command text");
    status_at(19, RCVCMD_STOPPED, RCVCMD_STOP_REQUEST, RCVCMD_START_ERROR_NONE);
    set_option(cmd, "misc-startcmd", "");
    set_option(cmd, "misc-stopcmd", "");
    result = rcvcmd_start(cmd, &io);
    CHECK(result.kind == RCVCMD_SUCCESS && result.seq == 21 && read_all(cmd) == 22,
          "start again with external commands cleared");
    rcvcmd_request_shutdown(cmd);
    last = rcvcmd_last_seq(cmd);
    result = rcvcmd_restart(cmd, &io);
    CHECK(result.kind == RCVCMD_SHUTTING_DOWN && result.seq == last && rcvcmd_last_seq(cmd) == last,
          "shutdown refusal publishes nothing");
    rcvcmd_shutdown(cmd, &io);
    CHECK(read_all(cmd) == 24, "shutdown uses stop events");
    status_at(22, RCVCMD_STOPPING, RCVCMD_STOP_REQUEST, RCVCMD_START_ERROR_NONE);
    status_at(23, RCVCMD_STOPPED, RCVCMD_STOP_REQUEST, RCVCMD_START_ERROR_NONE);
    rcvcmd_close(cmd);
    rcvcmd_close(cmd);
    rcvcmd_destroy(cmd);
#ifdef RCVCMD_TEST_THREADS
    CHECK(collector_joined == 1, "initialized collector joins exactly once");
#endif
    CHECK(unlink(config) == 0 && chdir(original) == 0 && rmdir(directory) == 0, "temporary config cleanup");
    free(original);
    printf("%s (%d failures)\n", fails ? "FAILED" : "PASSED", fails);
    return fails ? 1 : 0;
}
