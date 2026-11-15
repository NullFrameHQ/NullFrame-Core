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

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "terrain_sampling.h"
#include "terrain_math.h"
#include "avx512_kernels.h"
#if defined(__AVX2__) || defined(_M_AVX2)
#include <immintrin.h>
#define MYELIN_HAVE_AVX2 1
#else
#define MYELIN_HAVE_AVX2 0
#endif

extern float getSpline(const Spline *sp, const float *vals);
volatile int g_correction_amp = 4;
volatile int g_biome_step = 0;


/* Quintic fade: C2-continuous easing used by the smooth correction.
 * fade(0)=0, fade(1)=1, and both first derivatives are 0 — no slope
 * snaps at lattice points, so the corrected terrain rolls instead of
 * cratering. Shared with the bench reference (bench.c #includes us). */


/* ── HIGH-TERRAIN BIOMES (staircase gate) ──
 * Only these biomes get the 2-3 block staircase steps — the user's "depth
 * layers": hills, mountains, peaks, slopes. Normal terrain (plains, forest,
 * taiga flats, etc.) stays with the smooth bilinear ramp. Matches the
 * biomenoise.c depth values (hills 0.45, mountains 1.00). */
static int is_high_terrain_biome(int biome_id) {
    switch (biome_id) {
        /* Hills (depth ~0.45) */
        case wooded_hills: case birch_forest_hills: case taiga_hills:
        case snowy_taiga_hills: case jungle_hills: case desert_hills:
        case swamp_hills: case mountain_edge:
        case giant_tree_taiga_hills: case giant_spruce_taiga_hills:
        case tall_birch_hills: case dark_forest_hills: case bamboo_jungle_hills:
        /* Mountains / peaks / slopes (depth 0.8-1.0) */
        case mountains: case wooded_mountains: case gravelly_mountains:
        case modified_gravelly_mountains: case snowy_mountains:
        case shattered_savanna: case shattered_savanna_plateau:
        case snowy_slopes: case jagged_peaks: case frozen_peaks: case stony_peaks:
            return 1;
        default:
            return 0;
    }
}

/* Dominant (most common) of the 4 cell corners — the cell's biome identity. */
static int dominant_corner_biome(int b00, int b10, int b01, int b11) {
    int bs[4] = {b00, b10, b01, b11};
    int best = bs[0], best_count = 0;
    for (int i = 0; i < 4; i++) {
        int cnt = 0;
        for (int j = 0; j < 4; j++) if (bs[j] == bs[i]) cnt++;
        if (cnt > best_count) { best_count = cnt; best = bs[i]; }
    }
    return best;
}

/*
 * Quantise steep hill and mountain cells into two- or three-block height steps.
 * The phase uses world coordinates so adjacent chunks choose the same levels.
 * Low terrain, flat cells and sampling steps below 16 are left unchanged.
 */
void apply_staircase_shaping(float *out_heights, int base_x, int base_z,
                                    int sampling_step,
                                    float h00, float h10, float h01, float h11,
                                    int b00, int b10, int b01, int b11) {
    if (sampling_step < 16) return;
    /* Biome gate: the cell must be a hill/mountain biome. */
    if (!is_high_terrain_biome(dominant_corner_biome(b00, b10, b01, b11))) return;
    /* Cell relief = max corner delta. Hills only: < 3 blocks relief = flat. */
    float dx = h10 - h00; if (dx < 0) dx = -dx;
    float dx2 = h11 - h01; if (dx2 < 0) dx2 = -dx2;
    float dz = h01 - h00; if (dz < 0) dz = -dz;
    float dz2 = h11 - h10; if (dz2 < 0) dz2 = -dz2;
    float steep = dx;
    if (dx2 > steep) steep = dx2;
    if (dz > steep) steep = dz;
    if (dz2 > steep) steep = dz2;
    if (steep < 3.0f) return;
    int step = (steep >= 6.0f) ? 3 : 2;
    const int LATTICE = 32;
    for (int lz = 0; lz < 16; lz++) {
        int wz = base_z + lz;
        int gz = floor_div_world(wz, LATTICE) * LATTICE;
        float fz = (float)(wz - gz) / (float)LATTICE;
        float fzv = quintic_fade(fz);
        for (int lx = 0; lx < 16; lx++) {
            int wx = base_x + lx;
            int idx = lz * 16 + lx;
            float h = out_heights[idx];
            if (h <= 65.0f) continue;  /* keep sea level smooth */
            int gx = floor_div_world(wx, LATTICE) * LATTICE;
            float fx = (float)(wx - gx) / (float)LATTICE;
            float fxv = quintic_fade(fx);
            float p00 = value_noise_hash(gx, gz);
            float p10 = value_noise_hash(gx + LATTICE, gz);
            float p01 = value_noise_hash(gx, gz + LATTICE);
            float p11 = value_noise_hash(gx + LATTICE, gz + LATTICE);
            float phase = bilerp(p00, p10, p01, p11, fxv, fzv) * (float)step;
            float q = (float)round_to_int((h - phase) / (float)step) * (float)step + phase;
            out_heights[idx] = q;
        }
    }
}


int map_overworld_height_sample(Generator *g,
                                       const SurfaceNoise *sn,
                                       int quarter_x,
                                       int quarter_z) {
    float y = 64.0f;
    if (mapApproxHeight(&y, NULL, g, sn, quarter_x, quarter_z, 1, 1) != 0) {
        return 64;
    }
    return round_to_int(y);
}

/* ════════════════════════════════════════════════════════════════════════════
 *  1:1 Block-Scale Sampling (the fix for "dull terrain")
 *
 *  The problem: mapApproxHeight samples at 1:4 biome scale (integer coords).
 *  This produces smooth, dull terrain with no high-frequency detail.
 *
 *  The fix: sample the climate noise directly at fractional biome coords
 *  (block_x / 4.0). The underlying DoublePerlin noise is continuous and can
 *  be sampled at any position. We replicate sampleBiomeNoise's logic but with
 *  double coordinates instead of int.
 *
 *  This gives us 16×16 height samples per chunk (1:1 block scale) instead of
 *  5×5 (1:4 biome scale). Terrain has real ridges, valleys, and coastlines.
 * ════════════════════════════════════════════════════════════════════════════ */

/*
 * Samples biome noise at double-precision coordinates.
 * Replicates sampleBiomeNoise() from biomenoise.c but accepts fractional
 * biome-scale coords instead of integers. This allows 1:1 block-scale sampling.
 *
 * Returns biome ID. If np != NULL, fills np[6] with climate parameters.
 */
int sample_biome_noise_double(const BiomeNoise *bn, int64_t *np,
                                      double x, int y, double z,
                                      uint64_t *dat, uint32_t sample_flags) {
    float t = 0, h = 0, c = 0, e = 0, d = 0, w = 0;
    double px = x, pz = z;

    if (!(sample_flags & SAMPLE_NO_SHIFT)) {
        px += sampleDoublePerlin(&bn->climate[NP_SHIFT], x, 0, z) * 4.0;
        pz += sampleDoublePerlin(&bn->climate[NP_SHIFT], z, x, 0) * 4.0;
    }

    c = sampleDoublePerlin(&bn->climate[NP_CONTINENTALNESS], px, 0, pz);
    e = sampleDoublePerlin(&bn->climate[NP_EROSION], px, 0, pz);
    w = sampleDoublePerlin(&bn->climate[NP_WEIRDNESS], px, 0, pz);

    if (!(sample_flags & SAMPLE_NO_DEPTH)) {
        float np_param[] = {
            c, e, -3.0f * (fabsf(fabsf(w) - 0.6666667f) - 0.33333334f), w,
        };
        double off = getSpline(bn->sp, np_param) + 0.015f;
        d = 1.0 - (y * 4) / 128.0 - 83.0/160.0 + off;
    }

    t = sampleDoublePerlin(&bn->climate[NP_TEMPERATURE], px, 0, pz);
    h = sampleDoublePerlin(&bn->climate[NP_HUMIDITY], px, 0, pz);

    int64_t l_np[6];
    int64_t *p_np = np ? np : l_np;
    p_np[0] = (int64_t)(10000.0f * t);
    p_np[1] = (int64_t)(10000.0f * h);
    p_np[2] = (int64_t)(10000.0f * c);
    p_np[3] = (int64_t)(10000.0f * e);
    p_np[4] = (int64_t)(10000.0f * d);
    p_np[5] = (int64_t)(10000.0f * w);

    int id = none;
    if (!(sample_flags & SAMPLE_NO_BIOME))
        id = climateToBiome(bn->mc, (const uint64_t*)p_np, dat);
    return id;
}

/*
 * Compute height depth and a surface biome from the same climate noise samples.
 * Height uses the calculated depth; biome lookup uses zero depth to exclude cave biomes.
 */
int sample_biome_noise_pair(const BiomeNoise *bn, double x, double z,
                                   int64_t *np_height, int want_biome) {
    float t = 0, h = 0, c = 0, e = 0, d = 0, w = 0;
    double px = x, pz = z;

    px += sampleDoublePerlin(&bn->climate[NP_SHIFT], x, 0, z) * 4.0;
    pz += sampleDoublePerlin(&bn->climate[NP_SHIFT], z, x, 0) * 4.0;

    c = sampleDoublePerlin(&bn->climate[NP_CONTINENTALNESS], px, 0, pz);
    e = sampleDoublePerlin(&bn->climate[NP_EROSION], px, 0, pz);
    w = sampleDoublePerlin(&bn->climate[NP_WEIRDNESS], px, 0, pz);

    float np_param[] = {
        c, e, -3.0f * (fabsf(fabsf(w) - 0.6666667f) - 0.33333334f), w,
    };
    double off = getSpline(bn->sp, np_param) + 0.015f;
    /* Height depth with y=0: 1.0 - (0*4)/128.0 - 83.0/160.0 + off */
    d = 1.0 - 83.0/160.0 + off;

    t = sampleDoublePerlin(&bn->climate[NP_TEMPERATURE], px, 0, pz);
    h = sampleDoublePerlin(&bn->climate[NP_HUMIDITY], px, 0, pz);

    int64_t l_np[6];
    int64_t *p_np = np_height ? np_height : l_np;
    p_np[0] = (int64_t)(10000.0f * t);
    p_np[1] = (int64_t)(10000.0f * h);
    p_np[2] = (int64_t)(10000.0f * c);
    p_np[3] = (int64_t)(10000.0f * e);
    p_np[4] = (int64_t)(10000.0f * d);
    p_np[5] = (int64_t)(10000.0f * w);

    if (!want_biome)
        return none;

    /* Use zero depth for surface biome lookup. */
    int64_t np_biome[6];
    np_biome[0] = p_np[0];
    np_biome[1] = p_np[1];
    np_biome[2] = p_np[2];
    np_biome[3] = p_np[3];
    np_biome[4] = 0;
    np_biome[5] = p_np[5];
    return climateToBiome(bn->mc, (const uint64_t*)np_biome, NULL);
}

/*
 * Per-thread cache for world-aligned height and biome corners.
 * Entries are invalidated when the generator seed changes.
 */
#define CORNER_CACHE_SLOTS 512

typedef struct {
    int64_t key;      /* packed quarter coords: ((int64)qx << 32) ^ (uint32)qz */
    float height;
    int biome;
    int valid;
} CornerSlot;

static TLS_STORAGE CornerSlot tls_corner_cache[CORNER_CACHE_SLOTS];
static TLS_STORAGE uint64_t tls_corner_gen = (uint64_t)-1;

static uint32_t corner_slot_hash(int64_t key) {
    uint64_t h = (uint64_t)key * 0x9E3779B97F4A7C15ULL;
    h ^= h >> 33;
    return (uint32_t)h;
}

/* Height + biome for a cell corner at quarter coords (qx, qz). A cache hit
 * skips the expensive climate noise entirely; misses compute via the merged
 * pair eval and are stored for future chunks. */
void corner_sample_cached(const Generator *g, int qx, int qz,
                                 const BulkNoiseCache *bc,
                                 float *out_h, int *out_b) {
    if (tls_corner_gen != g->seed) {
        tls_corner_gen = g->seed;
        for (int i = 0; i < CORNER_CACHE_SLOTS; i++) tls_corner_cache[i].valid = 0;
    }
    int64_t key = ((int64_t)qx << 32) ^ (qz & 0xFFFFFFFFL);
    uint32_t idx = corner_slot_hash(key) & (CORNER_CACHE_SLOTS - 1);
    CornerSlot *slot = &tls_corner_cache[idx];
    if (slot->valid && slot->key == key) {
        *out_h = slot->height;
        *out_b = slot->biome;
        return;
    }
    int64_t np[6];
    /* Always compute the biome so cached entries stay valid in both modes. */
    int biome = bc
        ? sample_biome_noise_pair_bulk(bc, &g->bn, (double)qx, (double)qz, np, 1)
        : sample_biome_noise_pair(&g->bn, (double)qx, (double)qz, np, 1);
    float height = (float)(np[NP_DEPTH] / 76.0);
    slot->key = key;
    slot->height = height;
    slot->biome = biome;
    slot->valid = 1;
    *out_h = height;
    *out_b = biome;
}

void map_approx_height_fine(float *out_heights, int *out_biomes,
                                     const Generator *g,
                                     int chunk_x, int chunk_z,
                                     int sampling_step,
                                     const BulkNoiseCache *bc) {
    const BiomeNoise *bn = &g->bn;
    int base_x = chunk_x * 16;
    int base_z = chunk_z * 16;

    /* Clamp terrain sample spacing to 1 through 128 blocks. */
    if (sampling_step < 1) sampling_step = 1;
    if (sampling_step > 128) sampling_step = 128;

    if (sampling_step >= 16) {
        /*
         * For steps of 16 and above, interpolate the world-aligned cell containing this chunk.
         * Neighbouring chunks reuse the same cell corners.
         */
        int cell_x = floor_div_world(base_x, sampling_step) * sampling_step;
        int cell_z = floor_div_world(base_z, sampling_step) * sampling_step;
        int cell_end_x = cell_x + sampling_step;
        int cell_end_z = cell_z + sampling_step;

        /* Fetch the four cell corners through the per-thread cache. */
        int qx0 = cell_x / 4;
        int qz0 = cell_z / 4;
        int qx1 = cell_end_x / 4;
        int qz1 = cell_end_z / 4;
        float h00, h10, h01, h11;
        int b00, b10, b01, b11;
        corner_sample_cached(g, qx0, qz0, bc, &h00, &b00);
        corner_sample_cached(g, qx1, qz0, bc, &h10, &b10);
        corner_sample_cached(g, qx0, qz1, bc, &h01, &b01);
        corner_sample_cached(g, qx1, qz1, bc, &h11, &b11);

        float dh_x = h10 - h00;
        float dh2_x = h11 - h01;
        /* Constant per-chunk phase: where the chunk sits inside its cell. */
        float phase_x = (float)(base_x - cell_x);
        float phase_z = (float)(base_z - cell_z);

        /*
         * Optional smooth correction on a world-aligned 16-block lattice.
         * Its strength follows g_correction_amp and the terrain step.
         */
        int corr_amp = g_correction_amp;
        float eff_corr_amp = 0.0f;
        float corr00 = 0.0f, corr10 = 0.0f, corr01 = 0.0f, corr11 = 0.0f;
        if (corr_amp > 0) {
            eff_corr_amp = (float)(corr_amp * (sampling_step >= 32 ? 3 : 2)) / 4.0f;
            corr00 = value_noise_hash(base_x, base_z);
            corr10 = value_noise_hash(base_x + 16, base_z);
            corr01 = value_noise_hash(base_x, base_z + 16);
            corr11 = value_noise_hash(base_x + 16, base_z + 16);
        }

#if MYELIN_HAVE_AVX2
        /* AVX-512 first (runtime check): one 16-wide op per row instead of two 8-wide ops. Falls back to AVX2 below when unsupported. */
        if (om_cpu_has_avx512()) {
            om_fill_chunk16_avx512(out_heights, h00, h10, h01, h11,
                                   dh_x, dh2_x, phase_x, phase_z,
                                   sampling_step, eff_corr_amp,
                                   corr00, corr10, corr01, corr11);
        } else
        /*
         * AVX2 fill: eight columns per vector. Precompute column factors and broadcast row
         * factors, retaining the scalar path's multiply-then-add order.
         */
        {
            float tx_vals[16];
            float fade_u[16];
            for (int lx = 0; lx < 16; lx++) {
                tx_vals[lx] = (phase_x + (float)lx) / (float)sampling_step;
                fade_u[lx] = quintic_fade((float)lx / 16.0f);
            }
            __m256 vtwo = _mm256_set1_ps(2.0f);
            __m256 vone = _mm256_set1_ps(1.0f);
            __m256 vcorr00 = _mm256_set1_ps(corr00);
            __m256 vcorr10 = _mm256_set1_ps(corr10);
            __m256 vcorr01 = _mm256_set1_ps(corr01);
            __m256 vcorr11 = _mm256_set1_ps(corr11);
            __m256 vamp = _mm256_set1_ps(eff_corr_amp);
            for (int lz = 0; lz < 16; lz++) {
                float tz = (phase_z + (float)lz) / (float)sampling_step;
                float a = h00 + (h01 - h00) * tz;
                float b = dh_x + (dh2_x - dh_x) * tz;
                float fv = quintic_fade((float)lz / 16.0f);
                __m256 va = _mm256_set1_ps(a);
                __m256 vb = _mm256_set1_ps(b);
                __m256 vfv = _mm256_set1_ps(fv);
                int idx = lz * 16;
                int lx = 0;
                for (; lx + 8 <= 16; lx += 8) {
                    __m256 vtx = _mm256_loadu_ps(&tx_vals[lx]);
                    __m256 h = _mm256_add_ps(va, _mm256_mul_ps(vb, vtx)); /* a + b*tx */
                    if (eff_corr_amp > 0.0f) {
                        __m256 vfu = _mm256_loadu_ps(&fade_u[lx]);
                        /* bilerp(corr00,corr10,corr01,corr11,fu,fv) — same op order */
                        __m256 hx0 = _mm256_add_ps(vcorr00,
                                _mm256_mul_ps(_mm256_sub_ps(vcorr10, vcorr00), vfu));
                        __m256 hx1 = _mm256_add_ps(vcorr01,
                                _mm256_mul_ps(_mm256_sub_ps(vcorr11, vcorr01), vfu));
                        __m256 n = _mm256_add_ps(hx0,
                                _mm256_mul_ps(_mm256_sub_ps(hx1, hx0), vfv));
                        /* (n*2 - 1) * amp — same op order as scalar */
                        __m256 delta = _mm256_mul_ps(
                                _mm256_sub_ps(_mm256_mul_ps(n, vtwo), vone), vamp);
                        h = _mm256_add_ps(h, delta);
                    }
                    _mm256_storeu_ps(&out_heights[idx + lx], h);
                }
                /* Scalar tail — kept for safety (16 % 8 == 0, so never hit). */
                for (; lx < 16; lx++) {
                    float tx = (phase_x + (float)lx) / (float)sampling_step;
                    float h = a + b * tx;
                    if (eff_corr_amp > 0.0f) {
                        float n = bilerp(corr00, corr10, corr01, corr11,
                                quintic_fade((float)lx / 16.0f), fv);
                        h += (n * 2.0f - 1.0f) * eff_corr_amp;
                    }
                    out_heights[idx + lx] = h;
                }
            }
        }
#else
        /* Scalar fallback — identical math (non-AVX2 builds). */
        for (int lz = 0; lz < 16; lz++) {
            float tz = (phase_z + (float)lz) / (float)sampling_step;
            float a = h00 + (h01 - h00) * tz;
            float b = dh_x + (dh2_x - dh_x) * tz;
            int idx = lz * 16;
            for (int lx = 0; lx < 16; lx++) {
                float tx = (phase_x + (float)lx) / (float)sampling_step;
                float h = a + b * tx;
                if (eff_corr_amp > 0.0f) {
                    float n = bilerp(corr00, corr10, corr01, corr11,
                            quintic_fade((float)lx / 16.0f),
                            quintic_fade((float)lz / 16.0f));
                    h += (n * 2.0f - 1.0f) * eff_corr_amp;
                }
                out_heights[idx + lx] = h;
            }
        }
#endif
        /*
         * Apply mountain staircase shaping. MYELIN_BENCH_NO_STAIRCASE omits this pass
         * from benchmark targets that explicitly define it.
         */
#ifndef MYELIN_BENCH_NO_STAIRCASE
        apply_staircase_shaping(out_heights, base_x, base_z, sampling_step,
                                h00, h10, h01, h11, b00, b10, b01, b11);
#endif

        /*
         * Choose the nearest corner biome by selecting the nearest X and Z sides separately.
         * Ties favour the first side, matching the row-major corner order.
         */
        /* BIOME PASS — decoupled from terrain sampling.
         * When g_biome_step > 0 and finer than terrain, sample biomes at
         * g_biome_step resolution for smooth borders. Otherwise, use the
         * fast nearest-corner method (zero fresh noise evals). */
        if (out_biomes) {
            int biome_step = g_biome_step;
            if (biome_step > 0 && biome_step < sampling_step) {
                /* Sample biomes on the finer biome grid while retaining the terrain height grid. */
                int bs = biome_step;
                for (int lz = 0; lz < 16; lz++) {
                    int wz = base_z + lz;
                    int bz_cell = (wz / bs) * bs;
                    int qbz = bz_cell / 4;
                    for (int lx = 0; lx < 16; lx++) {
                        int wx = base_x + lx;
                        int bx_cell = (wx / bs) * bs;
                        int qbx = bx_cell / 4;
                        int64_t np_biome[6];
                        int id = bc
                            ? sample_biome_noise_pair_bulk(bc, bn, (double)qbx, (double)qbz, np_biome, 1)
                            : sample_biome_noise_pair(bn, (double)qbx, (double)qbz, np_biome, 1);
                        out_biomes[lz * 16 + lx] = id;
                    }
                }
            } else {
                /* NEAREST-CORNER: fast, zero fresh samples (original path). */
                int corner_biome[4] = {b00, b10, b01, b11};
                int xside[16];
                int zside[16];
                for (int lx = 0; lx < 16; lx++) {
                    float qx = (float)(base_x + lx) / 4.0f;
                    float dx0 = qx - (float)qx0;
                    float dx1 = qx - (float)qx1;
                    xside[lx] = (dx1 * dx1 < dx0 * dx0) ? 1 : 0;
                }
                for (int lz = 0; lz < 16; lz++) {
                    float qz = (float)(base_z + lz) / 4.0f;
                    float dz0 = qz - (float)qz0;
                    float dz1 = qz - (float)qz1;
                    zside[lz] = (dz1 * dz1 < dz0 * dz0) ? 2 : 0;
                }
                for (int lz = 0; lz < 16; lz++) {
                    int idx = lz * 16;
                    int zs = zside[lz];
                    for (int lx = 0; lx < 16; lx++) {
                        out_biomes[idx + lx] = corner_biome[xside[lx] + zs];
                    }
                }
            }
        }
        return;
    }

    if (sampling_step == 1) {
        /* Full 1:1 sampling — current behavior, no interpolation */
        for (int lz = 0; lz < 16; lz++) {
            for (int lx = 0; lx < 16; lx++) {
                double bx = (base_x + lx) / 4.0;
                double bz = (base_z + lz) / 4.0;
                int64_t np_height[6];
                int id = bc
                    ? sample_biome_noise_pair_bulk(bc, bn, bx, bz, np_height, out_biomes != NULL)
                    : sample_biome_noise_pair(bn, bx, bz, np_height, out_biomes != NULL);
                float height = (float)(np_height[NP_DEPTH] / 76.0);
                int idx = lz * 16 + lx;
                out_heights[idx] = height;
                if (out_biomes) {
                    out_biomes[idx] = id;
                }
            }
        }
        return;
    }

    /* Sampled mode — sample at grid points, bilinear interpolate the rest */
    int grid_points = 16 / sampling_step;  /* 8, 4, or 2 */
    if (grid_points < 1) grid_points = 1;
    int samples_per_axis = grid_points + 1;
    float coarse_h[81];  /* max 9x9 for step=2 */
    int coarse_b[81];
    float coarse_bx[81];
    float coarse_bz[81];

    /* Sample the coarse grid */
    for (int sz = 0; sz < samples_per_axis; sz++) {
        for (int sx = 0; sx < samples_per_axis; sx++) {
            int lx = sx * sampling_step;
            int lz = sz * sampling_step;
            if (lx > 15) lx = 15;
            if (lz > 15) lz = 15;
            double bx = (base_x + lx) / 4.0;
            double bz = (base_z + lz) / 4.0;
            int64_t np_height[6];
            int id = sample_biome_noise_pair(bn, bx, bz, np_height, out_biomes != NULL);
            float height = (float)(np_height[NP_DEPTH] / 76.0);
            int idx = sz * samples_per_axis + sx;
            coarse_h[idx] = height;
            coarse_bx[idx] = (float)bx;
            coarse_bz[idx] = (float)bz;
            if (out_biomes) {
                coarse_b[idx] = id;
            }
        }
    }

    /*
     * Fill each sampled cell using bilinear interpolation. Row factors are shared across
     * columns, with h = a + b * tx. Keep operation order consistent across SIMD paths.
     */
    for (int gz = 0; gz < grid_points; gz++) {
        int z0 = gz * sampling_step;
        int z1 = (gz + 1) * sampling_step;
        if (z1 > 16) z1 = 16;
        int row0 = gz * samples_per_axis;
        int row1 = (gz + 1) * samples_per_axis;
        for (int gx = 0; gx < grid_points; gx++) {
            int x0 = gx * sampling_step;
            int x1 = (gx + 1) * sampling_step;
            if (x1 > 16) x1 = 16;
            int cell_w = x1 - x0;
            float h00 = coarse_h[row0 + gx];
            float h10 = coarse_h[row0 + gx + 1];
            float h01 = coarse_h[row1 + gx];
            float h11 = coarse_h[row1 + gx + 1];
            float dh_x = h10 - h00;
            float dh2_x = h11 - h01;
            for (int lz = z0; lz < z1; lz++) {
                float tz = (float)(lz - z0) / (float)sampling_step;
                float a = h00 + (h01 - h00) * tz;
                float b = dh_x + (dh2_x - dh_x) * tz;
                int idx = lz * 16 + x0;
#if MYELIN_HAVE_AVX2
                if (om_cpu_has_avx512()) {
                    /* AVX-512: single masked 16-wide op covers any cell_w 1..16 */
                    om_fill_cell_avx512(out_heights, idx, cell_w, sampling_step, a, b);
                    continue;
                }
                if (cell_w >= 8) {
                    /* 8 output columns per lane, mul+add (AVX2-only, no FMA dep) */
                    int col = 0;
                    __m256 bv = _mm256_set1_ps(b);
                    __m256 av = _mm256_set1_ps(a);
                    for (; col + 8 <= cell_w; col += 8) {
                        /* tx uses the column offset inside this cell, not its offset inside the chunk. */
                        __m256 tx = _mm256_set_ps(
                                (float)(col + 7) / (float)sampling_step,
                                (float)(col + 6) / (float)sampling_step,
                                (float)(col + 5) / (float)sampling_step,
                                (float)(col + 4) / (float)sampling_step,
                                (float)(col + 3) / (float)sampling_step,
                                (float)(col + 2) / (float)sampling_step,
                                (float)(col + 1) / (float)sampling_step,
                                (float)(col) / (float)sampling_step);
                        __m256 h = _mm256_add_ps(_mm256_mul_ps(bv, tx), av);
                        _mm256_storeu_ps(&out_heights[idx + col], h);
                    }
                    for (; col < cell_w; col++) {
                        float tx = (float)col / (float)sampling_step;
                        out_heights[idx + col] = a + b * tx;
                    }
                    continue;
                }
#endif
                for (int lx = x0; lx < x1; lx++) {
                    float tx = (float)(lx - x0) / (float)sampling_step;
                    out_heights[idx + (lx - x0)] = a + b * tx;
                }
            }
        }
    }

    if (out_biomes) {
        /* Nearest sampled biome — identical logic, as a second pass. */
        int ncoarse = samples_per_axis * samples_per_axis;
        for (int lz = 0; lz < 16; lz++) {
            for (int lx = 0; lx < 16; lx++) {
                float nearest_dist = 1e30f;
                int nearest_biome = coarse_b[0];
                for (int ci = 0; ci < ncoarse; ci++) {
                    float dx = (float)((base_x + lx) / 4.0) - coarse_bx[ci];
                    float dz = (float)((base_z + lz) / 4.0) - coarse_bz[ci];
                    float dist = dx * dx + dz * dz;
                    if (dist < nearest_dist) {
                        nearest_dist = dist;
                        nearest_biome = coarse_b[ci];
                    }
                }
                out_biomes[lz * 16 + lx] = nearest_biome;
            }
        }
    }
}
