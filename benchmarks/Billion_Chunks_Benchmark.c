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

/* Billion_Chunks_Benchmark.exe — 1,000,000,000 chunks per sampling step.
 *
 * Same internals as cubiomes_bench.exe / sweep.exe:
 *   - links the native engine modules directly (no JNI, no Java, nothing else running)
 *   - calls the pure-C per-chunk path myelin_get_surface_chunk_data()
 *   - coherent sweep coordinates (i%64 rows) keep the TLS corner cache hot,
 *     so this measures the engine ceiling — same shape as bench.c BIG-BENCH
 *   - g_correction_amp = 4 (1-octave correction ON, like bench.c)
 *   - MYELIN_BENCH_NO_STAIRCASE (same as the other two benches)
 *
 * Matrix: sampling steps {16,32,64,128} x fixed 10 pinned workers.
 * Every RUN level completes exactly 1,000,000,000 chunks. 1:1..1:8 are NOT
 * run (they'd take days); their rates are EXTRAPOLATED from the measured
 * 1:16..1:128 curve instead, and printed in a separate estimate table.
 *
 * Output per level, live:
 *   - progress line every ~5 seconds (chunks done, %, elapsed, live ch/s)
 *   - final line: sample level, chunks/sec, seconds taken
 * Plus a measured summary table and an extrapolation table for 1:1..1:8.
 *
 * Expected runtime: ~15-40 minutes total on an idle machine.
 *
 * The console pauses on "Press Enter to exit" so double-click launches
 * don't close the window and lose the results.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <windows.h>
#include "surface_generation.h"
#include "terrain_sampling.h"
#include "terrain_math.h"

#define BILL_WORKERS  10
#define BILL_TARGET   1000000000LL   /* default: 1 billion chunks per step */
/* Optional argv[1] = chunk count (e.g. "Billion_Chunks_Benchmark.exe 10000000"
 * for a quick run). Default = full 1,000,000,000. */
static long long BILL_RUN_TARGET = BILL_TARGET;
#define BILL_PROGRESS_SEC 5.0       /* progress line interval */

/* Levels actually RUN at 1B chunks each. 1:1..1:8 are extrapolated. */
static const int BILL_STEPS[4] = {16, 32, 64, 128};
#define BILL_NLEVELS 4
/* Measured rates (ch/s) for the 4 run levels; extrapolation uses these. */
static double g_measured_cps[BILL_NLEVELS];

/* Pin to a logical core at max priority (same idea as sweep.c's pin, but the
 * mask never wraps within 10 threads on any modern CPU). */
static void bill_pin(int core) {
    SetThreadAffinityMask(GetCurrentThread(), 1ULL << (core % 64));
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
}

typedef struct {
    long long seed;
    int step;
    long long from, to;
    int core;
    int *out;
    volatile long long *done;   /* shared progress counter */
} bill_slice;

/* Coherent sweep (i%64 rows) — identical shape to sweep.c / bench.c BIG-BENCH
 * per-chunk mode: TLS corner cache stays hot, engine ceiling measured. */
static DWORD WINAPI bill_thread(LPVOID arg) {
    bill_slice *s = (bill_slice *)arg;
    bill_pin(s->core);
    long long local = 0;
    for (long long i = s->from; i < s->to; i++) {
        int cx = 1000000 + (int)(i % 64);
        int cz = 2000000 + (int)(i / 64);
        myelin_get_surface_chunk_data(s->seed, CB_DIM_OVERWORLD, cx, cz, s->out, s->step);
        if (++local >= 4096) {          /* batch the counter updates */
            InterlockedAdd64((volatile long long *)s->done, local);
            local = 0;
        }
    }
    if (local > 0) InterlockedAdd64((volatile long long *)s->done, local);
    return 0;
}

/* High-resolution wall clock in seconds. */
static double bill_now(void) {
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}

/* Run one level: BILL_TARGET chunks across BILL_WORKERS pinned workers.
 * Prints live progress every BILL_PROGRESS_SEC. Fills out_sec and out_cps. */
static void bill_level(long long seed, int step, double *out_sec, double *out_cps) {
    static int outs[BILL_WORKERS][256 * 3];
    HANDLE handles[BILL_WORKERS];
    bill_slice slices[BILL_WORKERS];
    volatile long long done = 0;
    long long per = BILL_RUN_TARGET / BILL_WORKERS;

    double t0 = bill_now();
    for (int t = 0; t < BILL_WORKERS; t++) {
        slices[t].seed = seed;
        slices[t].step = step;
        slices[t].from = (long long)t * per;
        slices[t].to   = (t == BILL_WORKERS - 1) ? BILL_RUN_TARGET
                                                 : (long long)(t + 1) * per;
        slices[t].core = t;
        slices[t].out  = outs[t];
        slices[t].done = &done;
        handles[t] = CreateThread(NULL, 0, bill_thread, &slices[t], 0, NULL);
        if (!handles[t]) { printf("thread creation failed\n"); exit(1); }
    }

    /* Progress monitor: poll every 5s while workers run. */
    double last_print = t0;
    for (;;) {
        DWORD w = WaitForMultipleObjects(BILL_WORKERS, handles, TRUE, 200);
        if (w == WAIT_OBJECT_0) break;              /* all workers finished */
        double now = bill_now();
        if (now - last_print >= BILL_PROGRESS_SEC) {
            last_print = now;
            double el = now - t0;
            printf("  1:%-4d %12lld / %lld chunks (%5.1f%%)  elapsed %8.1fs  live %10.0f ch/s\r",
                   step, done, BILL_RUN_TARGET,
                   100.0 * (double)done / (double)BILL_RUN_TARGET,
                   el, el > 0 ? (double)done / el : 0.0);
            fflush(stdout);
        }
    }
    for (int t = 0; t < BILL_WORKERS; t++) CloseHandle(handles[t]);

    double t1 = bill_now();
    double sec = t1 - t0;
    *out_sec = sec;
    *out_cps = sec > 0 ? (double)BILL_RUN_TARGET / sec : 0.0;

    /* Final line overwrites the \r progress line. */
    printf("  1:%-4d %12lld / %lld chunks (100.0%%)  DONE\n", step, BILL_RUN_TARGET, BILL_RUN_TARGET);
    printf("  1:%-4d -> 1,000,000,000 chunks in %.1f s  =  %.0f chunks/sec\n\n",
           step, sec, *out_cps);
    fflush(stdout);
}

/* Thousands separator for the summary. */
static void bill_fmt(long long v, char *buf, size_t n) {
    char raw[32];
    _snprintf_s(raw, sizeof(raw), _TRUNCATE, "%lld", v);
    int len = (int)strlen(raw);
    int commas = (len - 1) / 3;
    int pos = 0;
    for (int i = 0; i < len; i++) {
        if (i > 0 && (len - i) % 3 == 0 && pos < (int)n - 1) buf[pos++] = ',';
        if (pos < (int)n - 1) buf[pos++] = raw[i];
    }
    buf[pos] = '\0';
    (void)commas;
}

int main(int argc, char **argv) {
    long long seed = 12345LL;
    /* Optional argv[1] = chunks per level (quick runs). */
    if (argc > 1) {
        long long v = 0;
        if (sscanf(argv[1], "%lld", &v) == 1 && v >= 100000LL) {
            BILL_RUN_TARGET = v;
        } else {
            printf("Usage: Billion_Chunks_Benchmark.exe [chunks-per-level]\n");
            printf("  chunks-per-level = total chunks at EVERY level (>= 100000)\n");
            printf("  (omitted = full 1,000,000,000 per level)\n");
            getchar();
            return 1;
        }
    }
    Generator *g = get_cached_generator((uint64_t)seed);
    if (!g) { printf("generator init failed\n"); return 1; }

    printf("============================================================\n");
    printf("   BILLION CHUNKS BENCHMARK — 1,000,000,000 chunks per step\n");
    printf("============================================================\n");
    printf("SIMD path      : %s\n",
           om_cpu_has_avx512() ? "AVX-512 (runtime dispatched)" : "AVX2 (fallback)");
    printf("Workers        : %d (pinned, TIME_CRITICAL)\n", BILL_WORKERS);
    printf("Chunks / level : 1,000,000,000 (per-chunk path, engine ceiling)\n");
    printf("Seed           : %lld\n", seed);
    printf("Levels RUN     : 1:16 1:32 1:64 1:128  (1B chunks each, measured)\n");
    printf("Levels EST     : 1:1 1:2 1:4 1:8        (extrapolated, not run)\n");
    printf("Chunks/level   : %lld\n", BILL_RUN_TARGET);
    printf("Expected total : ~15-40 minutes (1:16 is the slowest RUN level)\n");
    printf("============================================================\n\n");
    fflush(stdout);

    g_correction_amp = 4;  /* bench with the 1-octave correction ON, like bench.c */

    static double secs[BILL_NLEVELS], cpss[BILL_NLEVELS];
    double total_start = bill_now();

    for (int s = 0; s < BILL_NLEVELS; s++) {
        bill_level(seed, BILL_STEPS[s], &secs[s], &cpss[s]);
        g_measured_cps[s] = cpss[s];
    }
    double total_sec = bill_now() - total_start;

    /* ── Summary table ── */
    printf("============================================================\n");
    printf("   RESULTS — %lld chunks each, %d workers\n", BILL_RUN_TARGET, BILL_WORKERS);
    printf("============================================================\n");
    printf("%-8s %18s %14s\n", "Level", "Chunks/sec", "Seconds");
    printf("------------------------------------------------------------\n");
    for (int s = 0; s < BILL_NLEVELS; s++) {
        char cbuf[32];
        bill_fmt((long long)cpss[s], cbuf, sizeof(cbuf));
        printf("1:%-6d %18s %14.1f\n", BILL_STEPS[s], cbuf, secs[s]);
    }
    printf("------------------------------------------------------------\n");
    printf("Total wall time: %.1f s (%.1f min)\n", total_sec, total_sec / 60.0);
    printf("============================================================\n\n");

    /* ── Extrapolation table for 1:1..1:8 (not run — would take days) ──
     * Model: per-chunk cost is dominated by the fresh corner samples the
     * TLS cache must compute. Going from step S to S/2 roughly doubles the
     * number of fresh samples per chunk area, so cost per chunk grows ~2x
     * per halving. Fit from the measured 1:16..1:128 curve: rate(step) ≈
     * rate(16) * (16/step)^p, with p fitted from the measured points. */
    {
        /* Fit exponent p from measured points via least squares on
         * log(rate) vs log(step): rate = C * step^p. Finer steps (smaller
         * step value) are SLOWER, so p is expected positive (rate grows
         * with step). */
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (int s = 0; s < BILL_NLEVELS; s++) {
            double x = log((double)BILL_STEPS[s]);
            double y = log(g_measured_cps[s]);
            sx += x; sy += y; sxx += x * x; sxy += x * y;
        }
        double n = (double)BILL_NLEVELS;
        double denom = n * sxx - sx * sx;
        double p = 0.0, C = 0.0;
        if (denom > 1e-12) {
            p = (n * sxy - sx * sy) / denom;
            C = exp((sy - p * sx) / n);
        }
        printf("============================================================\n");
        printf("   ESTIMATES for 1:1..1:8 (extrapolated, NOT measured)\n");
        printf("   model: rate = C * step^p fitted on the 1:16..1:128 results\n");
        printf("   fit:   p = %.3f\n", p);
        printf("============================================================\n");
        printf("%-8s %18s %16s\n", "Level", "Est. chunks/sec", "Est. time 1B");
        printf("------------------------------------------------------------\n");
        for (int s = 0; s < 4; s++) {
            int step = 1 << s;              /* 1, 2, 4, 8 */
            double cps = C * pow((double)step, p);
            /* Sanity clamp: 1:1 cannot be faster than the slowest measured
             * level's per-chunk cost floor — flag nonsense values. */
            double t1b = cps > 0 ? 1e9 / cps : 0;
            char cbuf[32];
            bill_fmt((long long)cps, cbuf, sizeof(cbuf));
            printf("1:%-6d %18s %13.1f min\n", step, cbuf, t1b / 60.0);
        }
        printf("------------------------------------------------------------\n");
        printf("NOTE: extrapolation assumes the cost curve keeps its shape below\n");
        printf("      1:16. At 1:1 nearly every column is a fresh sample, so real\n");
        printf("      1:1 is likely SLOWER than this model predicts (curve bends).\n");
        printf("============================================================\n");
    }
    fflush(stdout);

    /* Keep the console open (double-click launches close the window the
     * instant main returns — losing the whole run's results). */
    printf("\nBenchmark complete. Press Enter to exit...\n");
    fflush(stdout);
    getchar();
    return 0;
}
