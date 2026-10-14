/*------------------------------------------------------------------------------
 * unit test : trace sink levels, formatting, concurrency and stream secrets.
 * Uses public APIs only, with no test hooks in the library.
 *-----------------------------------------------------------------------------*/
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "mrtklib/rtklib.h"

#define THREADS 8
#define CALLS 20000

typedef struct {
    int level;
    char text[MRTK_TRACE_SINK_MSGLEN];
    size_t length;
} message_t;

typedef struct {
    pthread_mutex_t lock;
    message_t* messages;
    int count, capacity, levels[3];
    int ftp_done;
    pthread_t ftp_thread;
} capture_t;

static const char* case_name;
static int fails;

#define CHECK(cond, msg)                                         \
    do {                                                         \
        if (!(cond)) {                                           \
            fprintf(stderr, "FAIL: %s: %s\n", case_name, (msg)); \
            fails++;                                             \
        }                                                        \
    } while (0)

static void capture(void* arg, int level, const char* msg) {
    capture_t* c = (capture_t*)arg;
    pthread_mutex_lock(&c->lock);
    if (c->count < c->capacity) {
        c->messages[c->count].level = level;
        c->messages[c->count].length = strlen(msg);
        snprintf(c->messages[c->count].text, MRTK_TRACE_SINK_MSGLEN, "%s", msg);
    }
    c->count++;
    if (level >= 1 && level <= 2) c->levels[level]++;
    if (strstr(msg, "execcmd error:")) {
        c->ftp_thread = pthread_self();
        c->ftp_done = 1;
    }
    pthread_mutex_unlock(&c->lock);
}

static int enable(mrtk_ctx_t* ctx, capture_t* c, int level) {
    CHECK(tracesink(ctx, capture, c, 2), "tracesink failed");
    CHECK(tracesinklevel(ctx, level), "tracesinklevel failed");
    return !fails;
}

static void message(capture_t* c, int index, int level, const char* text) {
    CHECK(index < c->count && index < c->capacity, "missing message");
    if (index >= c->count || index >= c->capacity) return;
    CHECK(c->messages[index].level == level, "incorrect message level");
    if (strcmp(c->messages[index].text, text)) {
        fprintf(stderr, "FAIL: %s: message %d: expected <%s>, got <%s>\n", case_name, index, text,
                c->messages[index].text);
        fails++;
    }
}

static void levels_without_file(mrtk_ctx_t* ctx, capture_t* c) {
    int level;
    if (!enable(ctx, c, 2)) return;
    for (level = -1; level <= 3; level++) {
        trace(ctx, level, "trace %d\n", level);
        tracet(NULL, level, "tracet %d\n", level);
    }
    CHECK(c->count == 4, "expected four level-1/2 messages");
    message(c, 0, 1, "trace 1\n");
    message(c, 1, 1, "tracet 1\n");
    message(c, 2, 2, "trace 2\n");
    message(c, 3, 2, "tracet 2\n");
}

static void levels_with_file(mrtk_ctx_t* ctx, capture_t* c) {
    char path[L_tmpnam + 64], text[4096], prefix[64], expected[128];
    FILE* fp;
    size_t n;
    int level, consumed = 0;
    double elapsed = -1.0;
#ifdef _WIN32
    CHECK(tmpnam(path) != NULL, "tmpnam failed");
#else
    int fd;
    strcpy(path, "/tmp/mrtk-trace-XXXXXX");
    fd = mkstemp(path);
    CHECK(fd >= 0, "mkstemp failed");
    if (fd < 0) return;
    close(fd);
#endif
    if (!enable(ctx, c, 2)) {
        remove(path);
        return;
    }
    traceopen(ctx, path);
    CHECK(ctx->trace_fp != NULL, "traceopen failed");
    tracelevel(ctx, 1);
    trace(ctx, 2, "sink only\n");
    traceclose(ctx);
    fp = fopen(path, "rb");
    CHECK(fp != NULL, "cannot read trace file");
    if (fp) {
        CHECK(fgetc(fp) == EOF, "file level 1 received level 2");
        fclose(fp);
    }
    CHECK(c->count == 1, "sink missed level 2 with file level 1");
    message(c, 0, 2, "sink only\n");
    CHECK(tracesinklevel(ctx, 1), "cannot set sink level 1");
    traceopen(ctx, path);
    tracelevel(ctx, 5);
    for (level = 1; level <= 3; level++) trace(ctx, level, "file %d\n", level);
    tracet(ctx, 2, "timed %d\n", 42);
    traceclose(ctx);
    /* text mode, like traceopen(): Windows writes CRLF */
    fp = fopen(path, "r");
    CHECK(fp != NULL, "cannot reopen trace file");
    if (fp) {
        n = fread(text, 1, sizeof(text) - 1, fp);
        text[n] = '\0';
        fclose(fp);
        CHECK(strncmp(text, "file 1\nfile 2\nfile 3\n", 21) == 0, "file text changed");
        if (n >= 21) {
            CHECK(sscanf(text + 21, "2 %lf: %n", &elapsed, &consumed) == 1 && consumed > 0,
                  "tracet prefix cannot be parsed");
            snprintf(prefix, sizeof(prefix), "%d %9.3f: ", 2, elapsed);
            snprintf(expected, sizeof(expected), "%stimed 42\n", prefix);
            CHECK(strcmp(text + 21, expected) == 0, "tracet prefix/text format changed");
        }
    }
    CHECK(c->count == 2, "sink with level 1 received other levels");
    message(c, 1, 1, "file 1\n");
    CHECK(remove(path) == 0, "cannot remove trace file");
}

static void level_change(mrtk_ctx_t* ctx, capture_t* c) {
    if (!enable(ctx, c, 1)) return;
    trace(ctx, 2, "disabled\n");
    CHECK(c->count == 0, "level 1 delivered level 2");
    CHECK(tracesinklevel(NULL, 2), "cannot enable level 2");
    trace(ctx, 2, "enabled\n");
    CHECK(c->count == 1, "level 2 was not enabled immediately");
    message(c, 0, 2, "enabled\n");
    CHECK(tracesinklevel(ctx, 0), "cannot turn sink off");
    trace(ctx, 1, "off\n");
    tracet(ctx, 2, "off\n");
    CHECK(c->count == 1, "off sink delivered messages");
}

static void register_rules(mrtk_ctx_t* ctx, capture_t* c) {
    CHECK(!tracesinklevel(ctx, 1), "level accepted without a sink");
    CHECK(!tracesink(ctx, NULL, c, 2), "NULL callback accepted");
    CHECK(!tracesink(ctx, capture, c, 0), "max_level 0 accepted");
    CHECK(!tracesink(ctx, capture, c, -1), "negative max_level accepted");
    CHECK(tracesink(NULL, capture, c, 2), "registration with global fallback failed");
    trace(ctx, 1, "initially off\n");
    tracet(ctx, 2, "initially off\n");
    CHECK(c->count == 0, "newly registered sink was not off");
    CHECK(tracesinklevel(ctx, 1), "cannot enable first sink");
    CHECK(!tracesink(ctx, capture, NULL, 5), "second registration accepted");
    CHECK(!tracesinklevel(ctx, 3), "above-max level accepted");
    CHECK(!tracesinklevel(ctx, -1), "negative level accepted");
    trace(ctx, 1, "first sink\n");
    trace(ctx, 2, "still disabled\n");
    CHECK(c->count == 1, "rejected settings changed the sink");
    message(c, 0, 1, "first sink\n");
    g_mrtk_ctx = NULL;
    CHECK(!tracesink(NULL, capture, c, 2), "missing context accepted");
    CHECK(!tracesinklevel(NULL, 1), "missing context level accepted");
    g_mrtk_ctx = ctx;
}

static void tracet_no_prefix(mrtk_ctx_t* ctx, capture_t* c) {
    if (!enable(ctx, c, 2)) return;
    tracet(ctx, 2, "value=%d %.2f %s\n", 42, 1.25, "text");
    CHECK(c->count == 1, "tracet message count");
    message(c, 0, 2, "value=42 1.25 text\n");
}

static void truncation(mrtk_ctx_t* ctx, capture_t* c) {
    char text[3001];
    int i;
    for (i = 0; i < 3000; i++) text[i] = 'a' + i % 26;
    text[3000] = '\0';
    if (!enable(ctx, c, 2)) return;
    trace(ctx, 1, "%s", text);
    tracet(ctx, 2, "%s", text);
    CHECK(c->count == 2, "long messages not delivered");
    for (i = 0; i < c->count && i < 2; i++) {
        CHECK(c->messages[i].text[MRTK_TRACE_SINK_MSGLEN - 1] == '\0', "missing terminating NUL");
        CHECK(c->messages[i].length == MRTK_TRACE_SINK_MSGLEN - 1, "wrong callback message length");
        CHECK(strlen(c->messages[i].text) == MRTK_TRACE_SINK_MSGLEN - 1, "wrong truncated length");
        CHECK(memcmp(c->messages[i].text, text, MRTK_TRACE_SINK_MSGLEN - 1) == 0, "wrong truncated bytes");
    }
}

static void dumps_not_delivered(mrtk_ctx_t* ctx, capture_t* c) {
    nav_t* nav = (nav_t*)calloc(1, sizeof(nav_t));
    obsd_t obs = {0};
    double a = 1.0;
    uint8_t b = 1;
    CHECK(nav != NULL, "nav allocation failed");
    if (!nav) return;
    /* one record of each kind, so that every dump function has rows to write */
    nav->eph = (eph_t*)calloc(1, sizeof(eph_t));
    nav->geph = (geph_t*)calloc(1, sizeof(geph_t));
    nav->seph = (seph_t*)calloc(1, sizeof(seph_t));
    nav->peph = (peph_t*)calloc(1, sizeof(peph_t));
    nav->pclk = (pclk_t*)calloc(1, sizeof(pclk_t));
    CHECK(nav->eph && nav->geph && nav->seph && nav->peph && nav->pclk, "nav record allocation failed");
    nav->n = nav->ng = nav->ns = nav->ne = nav->nc = 1;
    if (!fails && enable(ctx, c, 2)) {
        tracemat(ctx, 1, &a, 1, 1, 8, 2);
        traceobs(ctx, 1, &obs, 1);
        tracenav(ctx, 1, nav);
        tracegnav(ctx, 1, nav);
        tracehnav(ctx, 1, nav);
        tracepeph(ctx, 1, nav);
        tracepclk(ctx, 1, nav);
        traceb(ctx, 1, &b, 1);
        CHECK(c->count == 0, "dump delivered to sink");
    }
    free(nav->eph);
    free(nav->geph);
    free(nav->seph);
    free(nav->peph);
    free(nav->pclk);
    free(nav);
}

typedef struct {
    mrtk_ctx_t* ctx;
    atomic_int stop, error;
} thread_arg_t;

static void* writer(void* arg) {
    thread_arg_t* a = (thread_arg_t*)arg;
    int i;
    for (i = 0; i < CALLS; i++) {
        trace(a->ctx, 1, "thread %d\n", i);
        if (i % 100 == 0) trace(NULL, 2, "optional %d\n", i);
    }
    return NULL;
}

static void* toggle(void* arg) {
    thread_arg_t* a = (thread_arg_t*)arg;
    while (!atomic_load_explicit(&a->stop, memory_order_relaxed)) {
        if (!tracesinklevel(a->ctx, 1) || !tracesinklevel(a->ctx, 2))
            atomic_store_explicit(&a->error, 1, memory_order_relaxed);
    }
    return NULL;
}

static void threads(mrtk_ctx_t* ctx, capture_t* c) {
    pthread_t workers[THREADS], setter;
    thread_arg_t arg;
    int i, started = 0, rc;
    if (!enable(ctx, c, 1)) return;
    arg.ctx = ctx;
    atomic_init(&arg.stop, 0);
    atomic_init(&arg.error, 0);
    rc = pthread_create(&setter, NULL, toggle, &arg);
    CHECK(rc == 0, "toggle pthread_create failed");
    if (rc) return;
    for (i = 0; i < THREADS; i++) {
        rc = pthread_create(&workers[i], NULL, writer, &arg);
        CHECK(rc == 0, "writer pthread_create failed");
        if (rc) break;
        started++;
    }
    for (i = 0; i < started; i++) CHECK(pthread_join(workers[i], NULL) == 0, "writer join failed");
    atomic_store_explicit(&arg.stop, 1, memory_order_relaxed);
    CHECK(pthread_join(setter, NULL) == 0, "toggle join failed");
    CHECK(!atomic_load_explicit(&arg.error, memory_order_relaxed), "concurrent level update failed");
    CHECK(c->levels[1] == THREADS * CALLS, "expected exactly 160000 level-1 messages");
    CHECK(c->levels[2] >= 0 && c->levels[2] <= THREADS * (CALLS / 100), "level-2 count out of bounds");
    CHECK(c->count == c->levels[1] + c->levels[2], "unexpected message levels");
    CHECK(c->count <= c->capacity, "capture capacity exceeded");
}

static void check_secrets(capture_t* c, int ntrip) {
    int i, found = 0;
    for (i = 0; i < c->count && i < c->capacity; i++) {
        const char* msg = c->messages[i].text;
        if (strstr(msg, "secretpw") || (ntrip && strstr(msg, "user"))) {
            fprintf(stderr, "FAIL: %s: secret in level-%d message: %s", case_name, c->messages[i].level, msg);
            fails++;
        }
        if (strstr(msg, ntrip ? "openntripc: no mountpoint" : "execcmd error:")) {
            found = 1;
            if (ntrip)
                CHECK(strstr(msg, "port=2101\n") != NULL, "missing caster port");
            else
                CHECK(strstr(msg, "ftp://127.0.0.1/file") && strstr(msg, "ret="), "missing source URL/return value");
        }
    }
    CHECK(found, "expected stream failure message missing");
}

static void ntripc_secret(mrtk_ctx_t* ctx, capture_t* c) {
    stream_t stream;
    if (!enable(ctx, c, 2)) return;
    strinit(&stream);
    CHECK(!stropen(&stream, STR_NTRIPCAS, STR_MODE_RW, "user:secretpw@:2101"), "caster without mountpoint opened");
    strclose(&stream);
    pthread_mutex_destroy(&stream.lock);
    check_secrets(c, 1);
}

static int ftp_secret(mrtk_ctx_t* ctx, capture_t* c) {
#ifdef _WIN32
    (void)ctx;
    (void)c;
    return 77;
#else
    char dir[] = "/tmp/mrtk-ftp-XXXXXX", wget[128], errfile[128], go[128], *path, *saved;
    const char* oldpath = getenv("PATH");
    FILE* fp;
    stream_t stream;
    uint8_t buff[1024];
    uint32_t start;
    pthread_t worker;
    int done = 0, opened, started = 0, released = 0;
    if (!enable(ctx, c, 2)) return 1;
    CHECK(mkdtemp(dir) != NULL, "mkdtemp failed");
    if (fails) return 1;
    snprintf(wget, sizeof(wget), "%s/wget", dir);
    snprintf(errfile, sizeof(errfile), "%s/file.err", dir);
    snprintf(go, sizeof(go), "%s/go", dir);
    fp = fopen(wget, "w");
    CHECK(fp != NULL, "cannot create fake wget");
    if (!fp) {
        rmdir(dir);
        return 1;
    }
    /* The fake wget fails only once the go file exists (or after 20 s). The
       test creates it after strread() returned and the capture mutex was
       released once; the worker's callback takes that mutex before the worker
       writes the ftp state, which strread() reads without a common lock. */
    CHECK(fprintf(fp,
                  "#!/bin/sh\ni=0\nwhile [ ! -e '%s' ] && [ $i -lt 400 ]; do sleep 0.05; i=$((i + 1)); done\n"
                  "exit 1\n",
                  go) > 0,
          "cannot write fake wget");
    CHECK(fclose(fp) == 0, "cannot close fake wget");
    CHECK(chmod(wget, 0700) == 0, "cannot make fake wget executable");
    saved = oldpath ? strdup(oldpath) : NULL;
    path = (char*)malloc(strlen(dir) + (oldpath ? strlen(oldpath) : 0) + 2);
    CHECK(path != NULL && (!oldpath || saved != NULL), "PATH allocation failed");
    if (path && (!oldpath || saved)) {
        sprintf(path, "%s:%s", dir, oldpath ? oldpath : "");
        CHECK(setenv("PATH", path, 1) == 0, "cannot set PATH");
    }
    if (!fails) {
        strsetdir(dir);
        strinit(&stream);
        opened = stropen(&stream, STR_FTP, STR_MODE_R, "user:secretpw@127.0.0.1/file");
        CHECK(opened, "FTP stream did not open");
        if (opened) {
            start = tickget();
            while ((uint32_t)(tickget() - start) < 20000) {
                /* Read until readftp starts the worker (it then sets the stream
                   message), but not after: the worker writes the ftp state
                   without the stream lock. */
                if (!started) {
                    strread(&stream, buff, sizeof(buff));
                    started = stream.msg[0] != '\0';
                }
                pthread_mutex_lock(&c->lock);
                done = c->ftp_done;
                if (done) worker = c->ftp_thread;
                pthread_mutex_unlock(&c->lock);
                if (started && !released) {
                    fp = fopen(go, "w");
                    CHECK(fp != NULL, "cannot create go file");
                    if (fp) fclose(fp);
                    released = 1;
                }
                if (done) break;
                sleepms(10);
            }
            CHECK(done, "FTP failure message did not arrive within 20 s");
            /* Joining the worker captured by the callback avoids closeftp's race. */
            if (done) CHECK(pthread_join(worker, NULL) == 0, "FTP worker join failed");
            if (started && !done) {
                /* Do not close or destroy a context still used by the worker. */
                remove(errfile);
                remove(go);
                remove(wget);
                rmdir(dir);
                exit(1);
            }
        }
        strclose(&stream);
        pthread_mutex_destroy(&stream.lock);
        strsetdir("");
        if (done) check_secrets(c, 0);
    }
    if (saved)
        CHECK(setenv("PATH", saved, 1) == 0, "cannot restore PATH");
    else if (!oldpath)
        CHECK(unsetenv("PATH") == 0, "cannot restore absent PATH");
    free(path);
    free(saved);
    remove(errfile);
    remove(go);
    CHECK(remove(wget) == 0, "cannot remove fake wget");
    CHECK(rmdir(dir) == 0, "cannot remove FTP directory");
    return 0;
#endif
}

int main(int argc, char** argv) {
    mrtk_ctx_t* ctx;
    capture_t c = {0};
    int result = 0;
    if (argc != 2) {
        fprintf(stderr, "usage: %s case\n", argv[0]);
        return 1;
    }
    case_name = argv[1];
    c.capacity = !strcmp(case_name, "threads") ? THREADS * (CALLS + CALLS / 100) : 32;
    c.messages = (message_t*)malloc(c.capacity * sizeof(message_t));
    CHECK(c.messages != NULL, "capture allocation failed");
    CHECK(pthread_mutex_init(&c.lock, NULL) == 0, "capture mutex init failed");
    ctx = mrtk_ctx_create();
    CHECK(ctx != NULL, "context allocation failed");
    if (fails) return 1;
    g_mrtk_ctx = ctx;
    if (!strcmp(case_name, "levels_without_file"))
        levels_without_file(ctx, &c);
    else if (!strcmp(case_name, "levels_with_file"))
        levels_with_file(ctx, &c);
    else if (!strcmp(case_name, "level_change"))
        level_change(ctx, &c);
    else if (!strcmp(case_name, "register_rules"))
        register_rules(ctx, &c);
    else if (!strcmp(case_name, "tracet_no_prefix"))
        tracet_no_prefix(ctx, &c);
    else if (!strcmp(case_name, "truncation"))
        truncation(ctx, &c);
    else if (!strcmp(case_name, "dumps_not_delivered"))
        dumps_not_delivered(ctx, &c);
    else if (!strcmp(case_name, "threads"))
        threads(ctx, &c);
    else if (!strcmp(case_name, "ntripc_secret"))
        ntripc_secret(ctx, &c);
    else if (!strcmp(case_name, "ftp_secret"))
        result = ftp_secret(ctx, &c);
    else
        CHECK(0, "unknown case");
    mrtk_ctx_destroy(ctx);
    g_mrtk_ctx = NULL;
    pthread_mutex_destroy(&c.lock);
    free(c.messages);
    if (fails) return 1;
    if (result) return result;
    printf("PASS: %s\n", case_name);
    return 0;
}
