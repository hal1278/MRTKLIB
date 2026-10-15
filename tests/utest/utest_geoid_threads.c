/*------------------------------------------------------------------------------
 * unit test : concurrent geoid queries and model replacement
 * Uses explicit checks so the regression remains active under -DNDEBUG.
 *-----------------------------------------------------------------------------*/
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "mrtklib/mrtk_geoid.h"

#define NQUERY 4
#define NITER 20000
#define NCYCLE 2000
#define TOL 1E-8

static int fails;

#define CHECK(cond, msg)                        \
    do {                                        \
        if (!(cond)) {                          \
            fprintf(stderr, "FAIL: %s\n", msg); \
            fails++;                            \
        }                                       \
    } while (0)

typedef struct {
    atomic_int start;
    atomic_int readers; /* query threads that have started querying */
    atomic_int cycled;  /* model replacement finished */
    const char* file;
    int cycling;
    int cycle_failures;
} run_t;

typedef struct {
    run_t* run;
    double pos[2], expected, embedded;
    double first_bad;
    int first_iter, failures;
} query_t;

/* Bilinear interpolation is exact for (3*i + 5*j - 4000) centimetres.
 * The full grid stays in [-40, 39.17] m, with distinct adjacent cells. */
static int write_grid(const char* file) {
    FILE* fp = fopen(file, "wb");
    if (!fp) {
        return 0;
    }
    for (int j = 0; j < 721; j++) {
        uint8_t row[1440 * 2];
        for (int i = 0; i < 1440; i++) {
            uint16_t v = (uint16_t)(3 * i + 5 * j - 4000);
            row[2 * i] = (uint8_t)(v >> 8);
            row[2 * i + 1] = (uint8_t)v;
        }
        if (fwrite(row, sizeof(row), 1, fp) != 1) {
            fclose(fp);
            return 0;
        }
    }
    return fclose(fp) == 0;
}

static void wait_start(run_t* run) {
    while (!atomic_load(&run->start)) {
        sched_yield();
    }
}

static void* query_thread(void* arg) {
    query_t* q = arg;
    wait_start(q->run);
    /* With replacement, keep querying until it ends so that both overlap. */
    for (int i = 0; i < NITER || (q->run->cycling && !atomic_load(&q->run->cycled)); i++) {
        double h = geoidh(q->pos);
        if (i == 0) {
            atomic_fetch_add(&q->run->readers, 1);
        }
        if (!isfinite(h) || (fabs(h - q->expected) > TOL && (!q->run->cycling || fabs(h - q->embedded) > TOL))) {
            if (!q->failures) {
                q->first_bad = h;
                q->first_iter = i;
            }
            q->failures++;
        }
    }
    return NULL;
}

static void* cycle_thread(void* arg) {
    run_t* run = arg;
    wait_start(run);
    /* Start replacing only while every query thread is querying. */
    while (atomic_load(&run->readers) < NQUERY) {
        sched_yield();
    }
    for (int i = 0; i < NCYCLE; i++) {
        if (!opengeoid(GEOID_EGM96_M150, run->file)) {
            run->cycle_failures++;
        }
        sched_yield();
        closegeoid();
        sched_yield();
    }
    atomic_store(&run->cycled, 1);
    return NULL;
}

static void run_threads(query_t* queries, run_t* run) {
    pthread_t threads[NQUERY + 1];
    int created = 0;
    atomic_store(&run->readers, 0);
    atomic_store(&run->cycled, 0);
    for (int i = 0; i < NQUERY; i++) {
        queries[i].failures = 0;
        int err = pthread_create(&threads[created], NULL, query_thread, &queries[i]);
        CHECK(err == 0, "create query thread");
        if (err) {
            break;
        }
        created++;
    }
    int cycle_created = 0;
    if (run->cycling && created == NQUERY) {
        int err = pthread_create(&threads[created], NULL, cycle_thread, run);
        CHECK(err == 0, "create model replacement thread");
        cycle_created = err == 0;
    }
    if (!cycle_created) {
        atomic_store(&run->cycled, 1); /* nothing for the query threads to wait for */
    }
    atomic_store(&run->start, 1);
    for (int i = 0; i < created; i++) {
        CHECK(pthread_join(threads[i], NULL) == 0, "join query thread");
        query_t* q = &queries[i];
        if (q->failures) {
            fprintf(stderr,
                    "FAIL: cycling=%d pos={%.12f,%.12f} rad iteration=%d got=%.12f "
                    "file=%.12f embedded=%.12f (%d mismatches)\n",
                    run->cycling, q->pos[0], q->pos[1], q->first_iter, q->first_bad, q->expected, q->embedded,
                    q->failures);
            fails++;
        }
    }
    if (cycle_created) {
        CHECK(pthread_join(threads[created], NULL) == 0, "join model replacement thread");
        CHECK(run->cycle_failures == 0, "all model replacements succeeded");
    }
}

int main(void) {
    const double d2r = 3.1415926535897932 / 180.0;
    const double indices[NQUERY][2] = {{100.25, 200.75}, {101.75, 201.25}, {800.5, 400.25}, {1200.25, 600.5}};
    const char* tmpdir = getenv("TMPDIR");
    char dir[4096], file[4096];
    run_t run = {.start = ATOMIC_VAR_INIT(0)};
    query_t queries[NQUERY] = {0};

    int n = snprintf(dir, sizeof(dir), "%s/utest_geoid_threads.XXXXXX", tmpdir ? tmpdir : "/tmp");
    CHECK(n > 0 && n < (int)sizeof(dir), "temporary directory path fits");
    if (fails) {
        return 1;
    }
    CHECK(mkdtemp(dir) != NULL, "create temporary directory");
    if (fails) {
        return 1;
    }
    n = snprintf(file, sizeof(file), "%s/geoid.dac", dir);
    CHECK(n > 0 && n < (int)sizeof(file), "temporary file path fits");
    if (fails) {
        CHECK(rmdir(dir) == 0, "remove temporary directory");
        return 1;
    }
    CHECK(write_grid(file), "write synthetic EGM96 grid");
    if (fails) {
        goto cleanup;
    }
    run.file = file;
    closegeoid();
    for (int i = 0; i < NQUERY; i++) {
        query_t* q = &queries[i];
        q->run = &run;
        q->pos[0] = (90.0 - indices[i][1] * 0.25) * d2r;
        q->pos[1] = indices[i][0] * 0.25 * d2r;
        q->expected = (3.0 * indices[i][0] + 5.0 * indices[i][1] - 4000.0) * 0.01;
        q->embedded = geoidh(q->pos);
    }
    CHECK(opengeoid(GEOID_EGM96_M150, file), "open synthetic EGM96 grid");
    if (!fails) {
        run_threads(queries, &run);
    }
    closegeoid();
    if (!fails) {
        run.cycling = 1;
        atomic_store(&run.start, 0);
        run_threads(queries, &run);
    }
cleanup:
    closegeoid();
    CHECK(unlink(file) == 0, "remove temporary grid");
    CHECK(rmdir(dir) == 0, "remove temporary directory");
    if (fails) {
        fprintf(stderr, "%d check(s) FAILED\n", fails);
        return 1;
    }
    printf("all geoid thread checks passed (%d queries/phase, %d replacements)\n", NQUERY * NITER, NCYCLE);
    return 0;
}
