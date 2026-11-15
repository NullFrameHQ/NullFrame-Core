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

/* Temporary C-only benchmark + verification harness for the pure native
 * per-chunk cost. links the native engine modules so the static TLS caches and
 * the non-JNI myelin_get_surface_chunk_data are directly callable —
 * no JNI, no Java. Compiled via the temporary cubiomes_bench target in the
 * root CMakeLists (MSVC, Windows).
 *
 * Three jobs:
 *   VERIFY <8  — replicate the ORIGINAL pre-AVX2 fill and compare against the
 *                CURRENT chunk-local fill (steps 2/4/8 must stay identical).
 *   VERIFY 16/32 — replicate the Phase-2 WORLD-ALIGNED cell fill (with the
 *                1-octave correction) and compare — this IS the seam check:
 *                adjacent chunks must both equal the world-coordinate
 *                function, proving continuity across borders. Includes
 *                negative chunk coordinates (floor-div correctness).
 *   BENCH    — aggregate chunks/sec across BENCH_WORKERS threads (the real
 *              worker budget; 8 pinned workers for the 1M marketing number).
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <windows.h>
#include "surface_generation.h"
#include "terrain_sampling.h"
#include "terrain_math.h"
#include "quad_baker.h"

#define BENCH_WORKERS 8

/* ── Reference: ORIGINAL pre-AVX2 chunk-local fill (steps 2/4/8) ── */
static void reference_fill_local(const BiomeNoise *bn, int base_x, int base_z,
                                 int sampling_step, float *ref) {
    if (sampling_step > 8) sampling_step = 8;
    int grid_points = 16 / sampling_step;
    if (grid_points < 1) grid_points = 1;
    int samples_per_axis = grid_points + 1;
    float coarse_h[81];
    for (int sz = 0; sz < samples_per_axis; sz++) {
        for (int sx = 0; sx < samples_per_axis; sx++) {
            int lx = sx * sampling_step;
            int lz = sz * sampling_step;
            if (lx > 15) lx = 15;
            if (lz > 15) lz = 15;
            double bx = (base_x + lx) / 4.0;
            double bz = (base_z + lz) / 4.0;
            int64_t np_height[6];
            sample_biome_noise_double(bn, np_height, bx, 0, bz, NULL, SAMPLE_NO_BIOME);
            float height = (float)(np_height[NP_DEPTH] / 76.0);
            int idx = sz * samples_per_axis + sx;
            coarse_h[idx] = height;
        }
    }
    for (int lz = 0; lz < 16; lz++) {
        for (int lx = 0; lx < 16; lx++) {
            int gx = lx / sampling_step;
            int gz = lz / sampling_step;
            if (gx >= grid_points) gx = grid_points - 1;
            if (gz >= grid_points) gz = grid_points - 1;
            float tx = (float)(lx - gx * sampling_step) / (float)sampling_step;
            float tz = (float)(lz - gz * sampling_step) / (float)sampling_step;
            if (tx < 0) tx = 0; if (tx > 1) tx = 1;
            if (tz < 0) tz = 0; if (tz > 1) tz = 1;
            int i00 = gz * samples_per_axis + gx;
            int i10 = gz * samples_per_axis + (gx + 1);
            int i01 = (gz + 1) * samples_per_axis + gx;
            int i11 = (gz + 1) * samples_per_axis + (gx + 1);
            float top = coarse_h[i00] + (coarse_h[i10] - coarse_h[i00]) * tx;
            float bottom = coarse_h[i01] + (coarse_h[i11] - coarse_h[i01]) * tx;
            ref[lz * 16 + lx] = top + (bottom - top) * tz;
        }
    }
}

/* ── Reference: Phase-2 WORLD-ALIGNED cell fill (steps 16/32, +correction) ── */
static void reference_fill_world_aligned(const BiomeNoise *bn, int base_x, int base_z,
                                         int sampling_step, float *ref) {
    int cell_x = floor_div_world(base_x, sampling_step) * sampling_step;
    int cell_z = floor_div_world(base_z, sampling_step) * sampling_step;
    double bx0 = cell_x / 4.0, bz0 = cell_z / 4.0;
    double bx1 = (cell_x + sampling_step) / 4.0, bz1 = (cell_z + sampling_step) / 4.0;
    int64_t np[6];
    /* MUST capture the corner biomes too — the impl's staircase gate is now
     * biome-based (dominant corner biome must be a hill/mountain). The impl
     * uses corner_sample_cached -> sample_biome_noise_pair(want_biome=1) at
     * the same quarter coords; sample_biome_noise_double without
     * SAMPLE_NO_BIOME returns the identical biome ID (same y=0 depth). */
    int b00 = sample_biome_noise_double(bn, np, bx0, 0, bz0, NULL, 0);
    float h00 = (float)(np[NP_DEPTH] / 76.0);
    int b10 = sample_biome_noise_double(bn, np, bx1, 0, bz0, NULL, 0);
    float h10 = (float)(np[NP_DEPTH] / 76.0);
    int b01 = sample_biome_noise_double(bn, np, bx0, 0, bz1, NULL, 0);
    float h01 = (float)(np[NP_DEPTH] / 76.0);
    int b11 = sample_biome_noise_double(bn, np, bx1, 0, bz1, NULL, 0);
    float h11 = (float)(np[NP_DEPTH] / 76.0);

    float dh_x = h10 - h00, dh2_x = h11 - h01;
    float phase_x = (float)(base_x - cell_x);
    float phase_z = (float)(base_z - cell_z);

    /* Must mirror the impl: smooth 16-block-lattice value noise, quintic
     * fade, amplitude matched to the sampling step (±2 at 1:16, ±3 at 1:32). */
    int corr_amp = g_correction_amp;
    float eff_corr_amp = 0.0f;
    if (corr_amp > 0) {
        eff_corr_amp = (float)(corr_amp * (sampling_step >= 32 ? 3 : 2)) / 4.0f;
    }
    float corr00 = value_noise_hash(base_x, base_z);
    float corr10 = value_noise_hash(base_x + 16, base_z);
    float corr01 = value_noise_hash(base_x, base_z + 16);
    float corr11 = value_noise_hash(base_x + 16, base_z + 16);
    for (int lz = 0; lz < 16; lz++) {
        float tz = (phase_z + lz) / (float)sampling_step;
        float a = h00 + (h01 - h00) * tz;
        float b = dh_x + (dh2_x - dh_x) * tz;
        for (int lx = 0; lx < 16; lx++) {
            float tx = (phase_x + lx) / (float)sampling_step;
            float h = a + b * tx;
            if (eff_corr_amp > 0.0f) {
                float u = (float)lx / 16.0f;
                float v = (float)lz / 16.0f;
                float n = bilerp(corr00, corr10, corr01, corr11,
                        quintic_fade(u), quintic_fade(v));
                h += (n * 2.0f - 1.0f) * eff_corr_amp;
            }
            ref[lz * 16 + lx] = h;
        }
    }
    /* MUST mirror the impl's staircase post-pass or VERIFY reports a false
     * regression. Same gate (biome + relief), same phase lattice, same
     * sea-level guard. (Also compiled out when the bench disables staircase
     * so the impl and reference stay consistent.) */
#ifndef MYELIN_BENCH_NO_STAIRCASE
    apply_staircase_shaping(ref, base_x, base_z, sampling_step,
                            h00, h10, h01, h11, b00, b10, b01, b11);
#endif
}

typedef struct {
    long long seed;
    int step;
    int from;
    int to;
    int scrambled; /* 1 = random chunk order (realistic cache hit rate) */
    int core;      /* logical core to pin this worker to (-1 = no pin) */
    int *out;      /* private per-thread buffer */
} bench_slice;

/* Pin the calling thread to a single logical core at max priority. The
 * corner cache is per-thread TLS, so no migration = best cache locality. */
static void pin_worker(int core) {
    if (core >= 0) {
        SetThreadAffinityMask(GetCurrentThread(), 1ULL << (core % 8));
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    }
}

/* Simple deterministic LCG — shared across threads, racing is fine for a bench. */
static uint64_t bench_rng = 0x123456789ABCDEFULL;
static uint32_t bench_rand(void) {
    bench_rng = bench_rng * 6364136223846793005ULL + 1442695040888963407ULL;
    return (uint32_t)(bench_rng >> 33);
}

static DWORD WINAPI bench_thread(LPVOID arg) {
    bench_slice *s = (bench_slice *)arg;
    for (int i = s->from; i < s->to; i++) {
        int cx, cz;
        if (s->scrambled) {
            /* 256x256-chunk region: bigger than the 512-slot corner cache, so
             * this measures the realistic (priority-queue-like) hit rate. */
            uint32_t r = bench_rand();
            cx = 1000000 + (r % 256);
            cz = 2000000 + ((r >> 8) % 256);
        } else {
            cx = 1000000 + (i % 64);
            cz = 2000000 + (i / 64);
        }
        myelin_get_surface_chunk_data(s->seed, CB_DIM_OVERWORLD, cx, cz, s->out, s->step);
    }
    return 0;
}

/* Region thread: ONE region call per worker over its horizontal strip of a
 * 64×64-chunk region (s->from/s->to = row range). This is the Phase-3
 * production shape — each strip is a single JNI-shaped call. */
static DWORD WINAPI bench_region_thread(LPVOID arg) {
    bench_slice *s = (bench_slice *)arg;
    int region_w = 64;
    int rx = 1000000;
    int rz = 2000000 + s->from;
    int h = s->to - s->from;
    myelin_get_surface_region_data(s->seed, CB_DIM_OVERWORLD, rx, rz,
                                       region_w, h, s->step, s->out);
    return 0;
}

/* BIG-BENCH: per-chunk sweep, pinned. Coherent sweep (i%64 rows) keeps the
 * TLS corner cache hot — the engine ceiling for the per-chunk path. */
static DWORD WINAPI bench_big_perchunk_thread(LPVOID arg) {
    bench_slice *s = (bench_slice *)arg;
    pin_worker(s->core);
    for (int i = s->from; i < s->to; i++) {
        int cx = 1000000 + (i % 64);
        int cz = 2000000 + (i / 64);
        myelin_get_surface_chunk_data(s->seed, CB_DIM_OVERWORLD, cx, cz, s->out, s->step);
    }
    return 0;
}

/* BIG-BENCH: region-loop, pinned. One 64×64 region call per iteration — the
 * exact Phase-3 production shape. Each worker walks its own horizontal band
 * of adjacent regions (staggered start row), keeping the TLS corner cache
 * coherent across calls. The 12MB out buffer is reused every call. */
static DWORD WINAPI bench_big_region_thread(LPVOID arg) {
    bench_slice *s = (bench_slice *)arg;
    pin_worker(s->core);
    int region_chunks = 64 * 64;
    int rx = 1000000;
    int rz = 2000000 + (s->core % 8) * 64;
    int done = 0;
    while (done < (s->to - s->from)) {
        myelin_get_surface_region_data(s->seed, CB_DIM_OVERWORLD, rx, rz,
                                           64, 64, s->step, s->out);
        done += region_chunks;
        rx += 64;
    }
    return 0;
}

/* ── QUAD-BAKE worker: bake the shared region repeatedly into a private
 * quad buffer; accumulates the total quad count. Mirrors the production
 * shape (bake a region per worker, tile output is consumed downstream). */
typedef struct {
    const int *h, *t, *b;
    int rw, rh, ox, oz, cx, cz;
    QbTier *tiers;
    int iters;
    QbQuad *buf;
    long long *out_quads;
} qb_bench_args;

static DWORD WINAPI qb_bench_thread(LPVOID arg) {
    qb_bench_args *a = (qb_bench_args *)arg;
    int tier_counts[6] = {0};
    long long total = 0;
    for (int i = 0; i < a->iters; i++) {
        int count = 0;
        qb_bake_region(a->h, a->t, a->b, a->rw, a->rh, a->ox, a->oz, a->cx, a->cz,
                       a->tiers, 6, a->buf, 65536, &count, tier_counts);
        total += count;
    }
    *a->out_quads = total;
    return 0;
}

int main(void) {
    long long seed = 12345LL;
    Generator *g = get_cached_generator((uint64_t) seed);
    if (!g) { printf("generator init failed\n"); return 1; }

    /* Which SIMD path the fill kernels will take on THIS cpu. */
    printf("SIMD path: %s\n", om_cpu_has_avx512() ? "AVX-512 (runtime dispatched)"
                                                    : "AVX2 (fallback)");

    g_correction_amp = 4;  /* bench with the 1-octave correction ON */

    static int out[256 * 3];
    static float ref[256];

    /* ── VERIFY: chunk-local steps must stay identical to the original ── */
    {
        int vsteps[3] = {2, 4, 8};
        for (int s = 0; s < 3; s++) {
            int step = vsteps[s];
            double max_diff = 0.0;
            int worst_cx = 0, worst_cz = 0;
            const int n = 200;
            for (int i = 0; i < n; i++) {
                int cx = 1000000 + (i % 64);
                int cz = 2000000 + (i / 64);
                myelin_get_surface_chunk_data(seed, CB_DIM_OVERWORLD, cx, cz, out, step);
                reference_fill_local(&g->bn, cx * 16, cz * 16, step, ref);
                for (int k = 0; k < 256; k++) {
                    double diff = (double)out[k] - (double)ref[k];
                    if (diff < 0) diff = -diff;
                    if (diff > max_diff) { max_diff = diff; worst_cx = cx; worst_cz = cz; }
                }
            }
            printf("VERIFY local step=%d: max |diff| vs ORIGINAL = %.4f (chunk %d,%d) %s\n",
                   step, max_diff, worst_cx, worst_cz,
                   max_diff < 1.0 ? "OK — identical terrain" : "MISMATCH — regression!");
        }
    }

    /* ── VERIFY: world-aligned 16/32/64/128 vs reference, incl. NEGATIVE ──
     * Matching the reference (a pure function of world coords) at every
     * column IS the seam check: two adjacent chunks both equal the same
     * continuous function, so the border is seamless. */
    {
        int vsteps[4] = {16, 32, 64, 128};
        for (int s = 0; s < 4; s++) {
            int step = vsteps[s];
            double max_diff = 0.0;
            int worst_cx = 0, worst_cz = 0;
            /* 100 positive + 100 negative chunks (floor-div correctness) */
            const int n = 200;
            for (int i = 0; i < n; i++) {
                int cx = (i < 100) ? 1000000 + i : -200000 - (i - 100);
                int cz = (i < 100) ? 2000000 + i : -300000 - (i - 100);
                myelin_get_surface_chunk_data(seed, CB_DIM_OVERWORLD, cx, cz, out, step);
                reference_fill_world_aligned(&g->bn, cx * 16, cz * 16, step, ref);
                for (int k = 0; k < 256; k++) {
                    double diff = (double)out[k] - (double)ref[k];
                    if (diff < 0) diff = -diff;
                    if (diff > max_diff) { max_diff = diff; worst_cx = cx; worst_cz = cz; }
                }
            }
            printf("VERIFY world-aligned step=%d: max |diff| vs REFERENCE = %.4f (chunk %d,%d) %s\n",
                   step, max_diff, worst_cx, worst_cz,
                   max_diff < 1.0 ? "OK — seamless, matches reference" : "MISMATCH!");
        }
    }

    /* ── VERIFY: REGION-in-one-pass == per-chunk path (all steps, incl.
     * NEGATIVE coords). The region function loops map_approx_height_fine in
     * row-major order, so this proves the region call produces byte-identical
     * output to the per-chunk path — the correctness gate for Phase 3. */
    {
        int vsteps[8] = {1, 2, 4, 8, 16, 32, 64, 128};
        for (int s = 0; s < 8; s++) {
            int step = vsteps[s];
            double max_diff = 0.0;
            int worst_cx = 0, worst_cz = 0;
            /* 3 region origins: positive, negative, mixed — each 4×4 = 16 chunks */
            const int origins[3][2] = {{1000000, 2000000}, {-200000, -300000}, {1000079, -200094}};
            for (int o = 0; o < 3; o++) {
                int rx = origins[o][0], rz = origins[o][1];
                static int region_out[16 * 256 * 3];
                myelin_get_surface_region_data(seed, CB_DIM_OVERWORLD, rx, rz, 4, 4, step, region_out);
                for (int lz = 0; lz < 4; lz++) {
                    for (int lx = 0; lx < 4; lx++) {
                        int cx = rx + lx, cz = rz + lz;
                        static int per_chunk_out[256 * 3];
                        myelin_get_surface_chunk_data(seed, CB_DIM_OVERWORLD, cx, cz, per_chunk_out, step);
                        int *region_chunk = region_out + (lz * 4 + lx) * (256 * 3);
                        for (int k = 0; k < 256 * 3; k++) {
                            double diff = (double)region_chunk[k] - (double)per_chunk_out[k];
                            if (diff < 0) diff = -diff;
                            if (diff > max_diff) { max_diff = diff; worst_cx = cx; worst_cz = cz; }
                        }
                    }
                }
            }
            printf("VERIFY region step=%d: max |diff| vs per-chunk = %.4f (chunk %d,%d) %s\n",
                   step, max_diff, worst_cx, worst_cz,
                   max_diff < 0.001 ? "OK — byte-identical" : "MISMATCH!");
        }
    }

    /* ── BENCH: aggregate chunks/sec across BENCH_WORKERS threads ── */
    {
        int steps[5] = {1, 2, 8, 16, 32};
        int counts[5] = {10000, 20000, 50000, 50000, 50000};
        static int outs[BENCH_WORKERS][256 * 3];
        for (int s = 0; s < 5; s++) {
            int step = steps[s];
            int chunks = counts[s];
            int per = chunks / BENCH_WORKERS;
            HANDLE handles[BENCH_WORKERS];
            bench_slice slices[BENCH_WORKERS];
            clock_t t0 = clock();
            for (int t = 0; t < BENCH_WORKERS; t++) {
                slices[t].seed = seed;
                slices[t].step = step;
                slices[t].scrambled = 0;
                slices[t].from = t * per;
                slices[t].to = (t == BENCH_WORKERS - 1) ? chunks : (t + 1) * per;
                slices[t].out = outs[t];
                handles[t] = CreateThread(NULL, 0, bench_thread, &slices[t], 0, NULL);
            }
            WaitForMultipleObjects(BENCH_WORKERS, handles, TRUE, INFINITE);
            for (int t = 0; t < BENCH_WORKERS; t++) CloseHandle(handles[t]);
            clock_t t1 = clock();
            double sec = (double)(t1 - t0) / CLOCKS_PER_SEC;
            printf("C-only step=%d (%d workers): %d chunks in %.1f ms -> %.0f chunks/sec aggregate\n",
                   step, BENCH_WORKERS, chunks, sec * 1000.0, sec > 0 ? chunks / sec : 0.0);
        }
    }

    /* ── REGION BENCH: full region in one pass, split into worker strips.
     * Each worker does ONE region call over its strip (row-major inside C),
     * so this is the Phase-3 production shape: chunks/sec is the honest
     * number the in-game rectangle scheduler should approach. */
    {
        int steps[5] = {1, 4, 8, 16, 32};
        int region_w = 64, region_h = 64;  /* 4096 chunks split over BENCH_WORKERS */
        int chunks = region_w * region_h;
        /* Worst strip = ceil(64/BENCH_WORKERS)=8 rows × 64 = 512 chunks/worker. */
        static int outs[BENCH_WORKERS][13 * 64 * 256 * 3];
        for (int s = 0; s < 3; s++) {
            int step = steps[s];
            int per_h = (region_h + BENCH_WORKERS - 1) / BENCH_WORKERS;
            HANDLE handles[BENCH_WORKERS];
            bench_slice slices[BENCH_WORKERS];
            clock_t t0 = clock();
            for (int t = 0; t < BENCH_WORKERS; t++) {
                slices[t].seed = seed;
                slices[t].step = step;
                slices[t].scrambled = 0;
                slices[t].from = t * per_h;
                slices[t].to = (t == BENCH_WORKERS - 1) ? region_h : (t + 1) * per_h;
                slices[t].out = outs[t];
                handles[t] = CreateThread(NULL, 0, bench_region_thread, &slices[t], 0, NULL);
            }
            WaitForMultipleObjects(BENCH_WORKERS, handles, TRUE, INFINITE);
            for (int t = 0; t < BENCH_WORKERS; t++) CloseHandle(handles[t]);
            clock_t t1 = clock();
            double sec = (double)(t1 - t0) / CLOCKS_PER_SEC;
            printf("C-only REGION step=%d (%d workers, 1 call/worker): %d chunks in %.1f ms -> %.0f chunks/sec aggregate\n",
                   step, BENCH_WORKERS, chunks, sec * 1000.0, sec > 0 ? chunks / sec : 0.0);
        }
    }

    /* ── SHUFFLED ORDER (realistic cache hit rate) ── */
    {
        int steps[2] = {16, 32};
        int chunks = 50000;
        static int outs[BENCH_WORKERS][256 * 3];
        for (int s = 0; s < 2; s++) {
            int step = steps[s];
            int per = chunks / BENCH_WORKERS;
            HANDLE handles[BENCH_WORKERS];
            bench_slice slices[BENCH_WORKERS];
            bench_rng = 0x9E3779B97F4A7C15ULL;
            clock_t t0 = clock();
            for (int t = 0; t < BENCH_WORKERS; t++) {
                slices[t].seed = seed;
                slices[t].step = step;
                slices[t].scrambled = 1;
                slices[t].from = t * per;
                slices[t].to = (t == BENCH_WORKERS - 1) ? chunks : (t + 1) * per;
                slices[t].out = outs[t];
                handles[t] = CreateThread(NULL, 0, bench_thread, &slices[t], 0, NULL);
            }
            WaitForMultipleObjects(BENCH_WORKERS, handles, TRUE, INFINITE);
            for (int t = 0; t < BENCH_WORKERS; t++) CloseHandle(handles[t]);
            clock_t t1 = clock();
            double sec = (double)(t1 - t0) / CLOCKS_PER_SEC;
            printf("C-only step=%d shuffled (%d workers): %d chunks in %.1f ms -> %.0f chunks/sec aggregate\n",
                   step, BENCH_WORKERS, chunks, sec * 1000.0, sec > 0 ? chunks / sec : 0.0);
        }
    }

    /* ── BIG BENCH: 1,000,000 chunks across 8 PINNED workers ──
     * The marketing number: pure C, no JNI, no Java. Each worker is pinned
     * to its own logical core at TIME_CRITICAL priority. Two modes:
     *   per-chunk   — coherent sweep, the per-chunk engine ceiling
     *   region-loop — the exact Phase-3 production shape (one 64×64 call
     *                 per iteration, coherent TLS corner cache).
     * 1M chunks ≈ 1-2s at these rates — long enough to amortize turbo.
     * 1:64 = 4x fewer fresh corners than 1:32, 1:128 = 16x fewer. */
    {
        const int chunks = 1000000;
        int steps[4] = {16, 32, 64, 128};
        int per = chunks / BENCH_WORKERS;
        static int big_outs[BENCH_WORKERS][64 * 64 * 256 * 3];  /* ~12MB/thread, reused */
        for (int mode = 0; mode < 2; mode++) {
            int regionMode = (mode == 1);
            const char *modeName = regionMode ? "region-loop" : "per-chunk";
            for (int s = 0; s < 4; s++) {
                int step = steps[s];
                HANDLE handles[BENCH_WORKERS];
                bench_slice slices[BENCH_WORKERS];
                clock_t t0 = clock();
                for (int t = 0; t < BENCH_WORKERS; t++) {
                    slices[t].seed = seed;
                    slices[t].step = step;
                    slices[t].scrambled = 0;
                    slices[t].from = t * per;
                    slices[t].to = (t == BENCH_WORKERS - 1) ? chunks : (t + 1) * per;
                    slices[t].core = t;
                    slices[t].out = big_outs[t];
                    handles[t] = CreateThread(NULL, 0,
                            regionMode ? bench_big_region_thread : bench_big_perchunk_thread,
                            &slices[t], 0, NULL);
                }
                WaitForMultipleObjects(BENCH_WORKERS, handles, TRUE, INFINITE);
                for (int t = 0; t < BENCH_WORKERS; t++) CloseHandle(handles[t]);
                clock_t t1 = clock();
                double sec = (double)(t1 - t0) / CLOCKS_PER_SEC;
                printf("BIG-BENCH 1M step=%d (%d pinned workers, %s): %d chunks in %.1f ms -> %.0f chunks/sec aggregate\n",
                       step, BENCH_WORKERS, modeName, chunks,
                       sec * 1000.0, sec > 0 ? chunks / sec : 0.0);
            }
        }
    }

    /* ── QUAD-BAKE: canonical LoD quad baker — verify + bench ──
     * Consumes the region output (heights/types/biomes) and produces
     * canonical quads with per-corner texture (corner blend) + skirts.
     * VERIFY: byte layout, per-tier counts, height fidelity, UV bounds,
     * region coverage (sum of top-quad areas == region area).
     * BENCH: 8 workers, each baking the full region N times. */
    {
        int qstep = 32;
        int qrw = 64, qrh = 64;              /* 1024x1024 blocks */
        int qchunks = qrw * qrh;
        static int qregion[64 * 64 * 768];   /* ~12.5MB */
        myelin_get_surface_region_data(seed, CB_DIM_OVERWORLD, 1000000, 2000000,
                                           qrw, qrh, qstep, qregion);
        const int *qh = qregion;
        const int *qt = qregion + qchunks * 256;
        const int *qb = qregion + qchunks * 512;
        int qox = 1000000 * 16, qoz = 2000000 * 16;   /* region origin, blocks */
        int qcx = qox + 512, qcz = qoz + 512;         /* player center */
        QbTier qtiers[6] = {
            { 4,   48 },
            { 8,   96 },
            { 16,  192 },
            { 32,  384 },
            { 64,  INT_MAX },
            { 128, INT_MAX },   /* never fires inside a 1024-wide region */
        };
        static QbQuad qquads[65536];
        int qtier_counts[6] = {0};
        int qcount = 0;
        int qrc = qb_bake_region(qh, qt, qb, qrw, qrh, qox, qoz, qcx, qcz,
                                 qtiers, 6, qquads, 65536, &qcount, qtier_counts);
        printf("QUAD-BAKE rc=%d total_quads=%d (top+skirts)\n", qrc, qcount);
        printf("  sizeof(QbVertex)=%d (expect 28), sizeof(QbQuad)=%d (expect 112)\n",
               (int)sizeof(QbVertex), (int)sizeof(QbQuad));

        /* ── VERIFY pass ── */
        int vbad_layout = (sizeof(QbVertex) != 28 || sizeof(QbQuad) != 112) ? 1 : 0;
        int vbad_bounds = 0, vbad_uv = 0, vbad_deg = 0, vbad_height = 0;
        long long cover_sum = 0;
        int top_quads = 0;
        for (int i = 0; i < qcount; i++) {
            const QbQuad *q = &qquads[i];
            for (int k = 0; k < 4; k++) {
                const QbVertex *v = &q->v[k];
                if (v->x < qox || v->x > qox + 1024 || v->z < qoz || v->z > qoz + 1024) vbad_bounds++;
                if (v->u < 0.0f || v->u > 1.0f || v->v < 0.0f || v->v > 1.0f) vbad_uv++;
            }
            if (q->v[0].flags & QB_VFLAG_TOP) {
                top_quads++;
                float area = (q->v[1].x - q->v[0].x) * (q->v[3].z - q->v[0].z);
                if (area <= 0.0f) vbad_deg++;
                cover_sum += (long long)(area + 0.5f);
                /* corner y must equal the region height at that column (sea if water) */
                for (int k = 0; k < 4; k++) {
                    int lx = (int)q->v[k].x - qox, lz = (int)q->v[k].z - qoz;
                    if (lx < 0) lx = 0; if (lx > 1023) lx = 1023;
                    if (lz < 0) lz = 0; if (lz > 1023) lz = 1023;
                    int expect = qh[(lz / 16 * qrw + lx / 16) * 256 + (lz % 16) * 16 + (lx % 16)];
                    if (expect < QB_SEA_LEVEL) expect = QB_SEA_LEVEL;
                    if ((int)q->v[k].y != expect) vbad_height++;
                }
            }
        }
        long long expect_area = 1024LL * 1024LL;
        printf("  VERIFY: layout=%s bounds_bad=%d uv_bad=%d degenerate=%d height_bad=%d\n",
               vbad_layout ? "FAIL" : "OK", vbad_bounds, vbad_uv, vbad_deg, vbad_height);
        printf("  VERIFY: top_quads=%d coverage=%lld/%lld %s\n",
               top_quads, cover_sum, expect_area,
               cover_sum == expect_area ? "OK — region fully covered" : "MISMATCH");
        printf("  tiers (cell,maxdist):");
        for (int t = 0; t < 5; t++) printf(" %dx(<%d)=%d", qtiers[t].cell_blocks, qtiers[t].max_dist, qtier_counts[t]);
        printf("\n");

        /* ── BENCH: 8 workers x N full-region bakes ── */
        {
            const int N = 20000;
            int per = N / BENCH_WORKERS;
            HANDLE handles[BENCH_WORKERS];
            static QbQuad worker_bufs[BENCH_WORKERS][65536];
            static long long worker_quads[BENCH_WORKERS];
            clock_t t0 = clock();
            for (int t = 0; t < BENCH_WORKERS; t++) {
                worker_quads[t] = 0;
                handles[t] = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE) qb_bench_thread,
                                          &(qb_bench_args){
                                              qh, qt, qb, qrw, qrh, qox, qoz, qcx, qcz,
                                              qtiers, per, worker_bufs[t], &worker_quads[t]
                                          }, 0, NULL);
            }
            WaitForMultipleObjects(BENCH_WORKERS, handles, TRUE, INFINITE);
            for (int t = 0; t < BENCH_WORKERS; t++) CloseHandle(handles[t]);
            clock_t t1 = clock();
            double sec = (double)(t1 - t0) / CLOCKS_PER_SEC;
            long long total_quads = 0;
            for (int t = 0; t < BENCH_WORKERS; t++) total_quads += worker_quads[t];
            printf("QUAD-BAKE bench (%d workers, %d bakes each): %lld quads in %.1f ms -> %.0f quads/sec (%.1f MB/s)\n",
                   BENCH_WORKERS, per, total_quads, sec * 1000.0,
                   sec > 0 ? (double)total_quads / sec : 0.0,
                   sec > 0 ? (double)total_quads * 112.0 / sec / 1048576.0 : 0.0);
        }
    }
    return 0;
}
