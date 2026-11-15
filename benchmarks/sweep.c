/*
 * Copyright (C) 2026 NullFrame Core contributors
 *
 * This file is part of NullFrame Core.
 *
 * NullFrame Core is free software: you can redistribute it and/or modify it under the
 * terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, version 3 of the License.
 *
 * NullFrame Core is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with NullFrame Core. If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: LGPL-3.0-only
 */

/* Sweep.exe — worker-count × sampling-step scaling benchmark.
 *
 * links the native engine modules (same trick as bench.c) so the pure-C per-chunk
 * path — the gameplay default — is called directly. No JNI, no Java, no
 * server: this exe is the ONLY thing running.
 *
 * Matrix: sampling steps {1,2,4,8,16,32,64,128} × worker counts {1..10}.
 * ADAPTIVE cell sizing: each cell runs enough chunks to measure for ~0.4s
 * (min 20K, cap 4M) — slow cells (1:1 @ 1 thread) stay short, fast cells
 * (1:128 @ 10 threads) get millions of chunks so they never finish inside
 * one Windows clock tick (~15ms) and produce garbage numbers. Each worker
 * is pinned to its own logical core (full 64-bit mask — threads 9/10 land
 * on HT siblings, which is part of what this sweep measures). A short
 * warm-up pass runs first so turbo / cold-start doesn't skew early cells.
 *
 * Output: live per-cell lines, then a summary table + best-threads-per-step.
 *
 * NOTE: 1:1 at 1 thread takes ~4 minutes alone — the full sweep runs
 * ~20-30 minutes. That is expected, not a hang.
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <windows.h>
#include "surface_generation.h"
#include "terrain_sampling.h"
#include "terrain_math.h"

#define SWEEP_MAX_THREADS  10
#define SWEEP_TARGET_SEC   0.4     /* aim for ~0.4s per measured cell */
#define SWEEP_MIN_CHUNKS   20000   /* floor so slow cells still average out */
#define SWEEP_MAX_CHUNKS   4000000 /* cap so fast cells don't run forever */
#define WARMUP_CHUNKS      100000

static const int SWEEP_STEPS[8] = {1, 2, 4, 8, 16, 32, 64, 128};

/* Pin to a logical core at max priority. Unlike bench.c's pin (which wraps
 * at 8), this supports all 20 logical processors — wrapping would put two
 * sweep threads on one core and fake a bottleneck. */
static void sweep_pin(int core) {
    if (core >= 0) {
        SetThreadAffinityMask(GetCurrentThread(), 1ULL << (core % 64));
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    }
}

typedef struct {
    long long seed;
    int step;
    int from, to;
    int core;
    int *out;
} sweep_slice;

/* Coherent sweep (i%64 rows) — same shape as bench.c's per-chunk big bench:
 * the TLS corner cache stays hot, so this measures the engine ceiling. */
static DWORD WINAPI sweep_thread(LPVOID arg) {
    sweep_slice *s = (sweep_slice *)arg;
    sweep_pin(s->core);
    for (int i = s->from; i < s->to; i++) {
        int cx = 1000000 + (i % 64);
        int cz = 2000000 + (i / 64);
        myelin_get_surface_chunk_data(s->seed, CB_DIM_OVERWORLD, cx, cz, s->out, s->step);
    }
    return 0;
}

/* Runs one cell: `chunks` chunks across `threads` pinned workers.
 * Returns aggregate chunks/sec. */
static double sweep_cell(long long seed, int step, int threads, int chunks,
                         int outs[][256 * 3]) {
    int per = chunks / threads;
    HANDLE handles[SWEEP_MAX_THREADS];
    sweep_slice slices[SWEEP_MAX_THREADS];
    clock_t t0 = clock();
    for (int t = 0; t < threads; t++) {
        slices[t].seed = seed;
        slices[t].step = step;
        slices[t].from = t * per;
        slices[t].to = (t == threads - 1) ? chunks : (t + 1) * per;
        slices[t].core = t;
        slices[t].out = outs[t];
        handles[t] = CreateThread(NULL, 0, sweep_thread, &slices[t], 0, NULL);
    }
    WaitForMultipleObjects(threads, handles, TRUE, INFINITE);
    for (int t = 0; t < threads; t++) CloseHandle(handles[t]);
    clock_t t1 = clock();
    double sec = (double)(t1 - t0) / CLOCKS_PER_SEC;
    return sec > 0 ? chunks / sec : 0.0;
}

int main(void) {
    long long seed = 12345LL;
    Generator *g = get_cached_generator((uint64_t) seed);
    if (!g) { printf("generator init failed\n"); return 1; }

    printf("SIMD path: %s\n", om_cpu_has_avx512() ? "AVX-512 (runtime dispatched)"
                                                    : "AVX2 (fallback)");
    printf("Sweep matrix: 8 sampling steps x 10 thread counts (adaptive chunks/cell, ~0.4s each)\n");
    printf("Expected runtime: ~2 minutes.\n\n");
    fflush(stdout);

    g_correction_amp = 4;  /* bench with the 1-octave correction ON, like bench.c */

    static int outs[SWEEP_MAX_THREADS][256 * 3];
    static double results[8][SWEEP_MAX_THREADS];

    /* ── Warm-up: get turbo + caches hot so cell #1 isn't a cold-start lie ── */
    printf("Warmup started...\n");
    fflush(stdout);
    sweep_cell(seed, 8, 8, WARMUP_CHUNKS, outs);
    printf("Warmup ended.\n\n");
    fflush(stdout);

    /* ── The sweep ── */
    for (int t = 1; t <= SWEEP_MAX_THREADS; t++) {
        printf("Thread count = %d, sweep starting\n", t);
        fflush(stdout);
        for (int s = 0; s < 8; s++) {
            int step = SWEEP_STEPS[s];
            /* Adaptive cell size: estimate this cell's rate from the 1-thread
             * result (scales ~linearly with threads), then pick enough chunks
             * to measure for ~SWEEP_TARGET_SEC. The 1-thread row uses the
             * MIN floor (its rate isn't known yet — measured first). */
            double est = (t == 1) ? 0.0 : results[s][0] * t;
            int chunks = (est > 0.0) ? (int)(est * SWEEP_TARGET_SEC) : SWEEP_MIN_CHUNKS;
            if (chunks < SWEEP_MIN_CHUNKS) chunks = SWEEP_MIN_CHUNKS;
            if (chunks > SWEEP_MAX_CHUNKS) chunks = SWEEP_MAX_CHUNKS;
            chunks -= chunks % t;   /* divisible across workers */

            double cps = sweep_cell(seed, step, t, chunks, outs);
            results[s][t - 1] = cps;
            printf("  1:%d -> %d chunks, ch/s is %.0f\n", step, chunks, cps);
            fflush(stdout);
        }
        printf("\n");
        fflush(stdout);
    }

    /* ── Summary table (values in millions of chunks/sec) ── */
    printf("==== SUMMARY (chunks/sec, millions) ====\n");
    printf("step  ");
    for (int t = 1; t <= SWEEP_MAX_THREADS; t++) printf("%8dT", t);
    printf("\n");
    for (int s = 0; s < 8; s++) {
        printf("1:%-4d", SWEEP_STEPS[s]);
        for (int t = 0; t < SWEEP_MAX_THREADS; t++) {
            printf("%9.2f", results[s][t] / 1000000.0);
        }
        printf("\n");
    }

    /* ── Best thread count per step ── */
    printf("\n==== BEST (peak chunks/sec per sampling step) ====\n");
    for (int s = 0; s < 8; s++) {
        int best_t = 1;
        for (int t = 1; t < SWEEP_MAX_THREADS; t++) {
            if (results[s][t] > results[s][best_t - 1]) best_t = t + 1;
        }
        printf("BEST 1:%d -> %d threads (%.0f ch/s)\n",
               SWEEP_STEPS[s], best_t, results[s][best_t - 1]);
    }
    fflush(stdout);

    /* Keep the console open (double-click launches close the window the
     * instant main returns — losing 20+ minutes of results). */
    printf("\nSweep complete. Press Enter to exit...\n");
    fflush(stdout);
    getchar();
    return 0;
}
