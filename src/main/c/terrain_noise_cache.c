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
#include "terrain_noise_cache.h"

extern float getSpline(const Spline *sp, const float *vals);

/*
 * Evaluate the same gradient cases as noise.c's indexedLerp using coefficient tables.
 * Keep the arithmetic order consistent with the direct noise path.
 */
static inline double bulk_indexed_lerp(uint8_t idx, double a, double b, double c)
{
    static const int8_t ex[16] = { 1,-1, 1,-1, 1,-1, 1,-1, 0, 0, 0, 0, 1, 0,-1, 0 };
    static const int8_t ey[16] = { 1, 1,-1,-1, 0, 0, 0, 0, 1,-1, 1,-1, 1,-1, 1,-1 };
    static const int8_t ez[16] = { 0, 0, 0, 0, 1, 1,-1,-1, 1, 1,-1,-1, 0, 1, 0,-1 };
    unsigned i = idx & 0xfu;
    return ex[i] * a + ey[i] * b + ez[i] * c;
}

/* Precompute the 8 gradient indices for the SWAPPED shift call, which the
 * scalar code evaluates as sampleDoublePerlin(shift, z, x, 0) — a 3D Perlin
 * along the line (d1=z, d2=x, d3=0): h1 and h2 vary, h3 is the constant
 * floor(p->c). Same idx-chain shape, second lattice coord plays h2's role. */
static inline void bulk_corner_fill_y(uint8_t *g, const uint8_t *idx,
                                      int I, int K, uint8_t h3)
{
    uint8_t H1 = (uint8_t)I, H2 = (uint8_t)K;
    uint8_t a1 = (uint8_t)(idx[H1]     + H2);
    uint8_t b1 = (uint8_t)(idx[H1 + 1] + H2);
    uint8_t a2 = (uint8_t)(idx[a1]     + h3);
    uint8_t a3 = (uint8_t)(idx[a1 + 1] + h3);
    uint8_t b2 = (uint8_t)(idx[b1]     + h3);
    uint8_t b3 = (uint8_t)(idx[b1 + 1] + h3);
    g[0] = idx[a2];     /* v4.a -> l1 */
    g[1] = idx[a2 + 1]; /* v4.b -> l5 */
    g[2] = idx[b2];     /* v6.a -> l2 */
    g[3] = idx[b2 + 1]; /* v6.b -> l6 */
    g[4] = idx[a3];     /* v5.a -> l3 */
    g[5] = idx[a3 + 1]; /* v5.b -> l7 */
    g[6] = idx[b3];     /* v7.a -> l4 */
    g[7] = idx[b3 + 1]; /* v7.b -> l8 */
}

/* samplePerlin along the swapped-shift 3D line: d1 = z*lf + a, d2 = x*lf + b,
 * d3 = p->c (constant). Op-for-op identical to samplePerlin's normal path
 * (d2 != 0 here; the d2==0 degenerate case yields the same values anyway). */
static inline double bulk_sample_perlin_swap(const BulkOct *bo, const PerlinNoise *p,
                                             double x, double z)
{
    double lf = p->lacunarity;
    double d1 = z * lf + p->a;
    double d2 = x * lf + p->b;
    double d3 = p->c;
    double i1 = floor(d1), i2 = floor(d2);
    int gx = (int)i1 - bo->x0;
    int gy = (int)i2 - bo->z0;
    if ((unsigned)gx > (unsigned)(bo->gw - 2) ||
        (unsigned)gy > (unsigned)(bo->gh - 2))
        return samplePerlin(p, d1, d2, d3, 0, 0); /* safety fallback */

    double f1 = d1 - i1, f2 = d2 - i2;
    double f3 = d3 - floor(d3);
    double t1 = f1*f1*f1 * (f1 * (f1*6.0 - 15.0) + 10.0);
    double t2 = f2*f2*f2 * (f2 * (f2*6.0 - 15.0) + 10.0);
    double t3 = f3*f3*f3 * (f3 * (f3*6.0 - 15.0) + 10.0);
    const uint8_t *g = bo->g + ((size_t)gy * (size_t)bo->gw + (size_t)gx) * 8;

    double l1 = bulk_indexed_lerp(g[0], f1,     f2,   f3);
    double l5 = bulk_indexed_lerp(g[1], f1,     f2,   f3 - 1.0);
    double l2 = bulk_indexed_lerp(g[2], f1 - 1.0, f2,   f3);
    double l6 = bulk_indexed_lerp(g[3], f1 - 1.0, f2,   f3 - 1.0);
    double l3 = bulk_indexed_lerp(g[4], f1,     f2-1, f3);
    double l7 = bulk_indexed_lerp(g[5], f1,     f2-1, f3 - 1.0);
    double l4 = bulk_indexed_lerp(g[6], f1 - 1.0, f2-1, f3);
    double l8 = bulk_indexed_lerp(g[7], f1 - 1.0, f2-1, f3 - 1.0);

    l1 = lerp(t1, l1, l2); l3 = lerp(t1, l3, l4);
    l5 = lerp(t1, l5, l6); l7 = lerp(t1, l7, l8);
    l1 = lerp(t2, l1, l3); l5 = lerp(t2, l5, l7);
    return lerp(t3, l1, l5);
}

/* Build the swap-flavor corner caches for the shift parameter. First lattice
 * coord comes from the z range, second from the x range (d2 = x*lf + b). */
static void bulk_build_param_swap(BulkOctaveSet *sets, const DoublePerlinNoise *dp,
                                  double bx0, double bx1, double bz0, double bz1,
                                  double spacing)
{
    const double fB = 337.0 / 331.0;
    const double mtab[2] = { 1.0, fB };
    for (int ab = 0; ab < 2; ab++) {
        const OctaveNoise *on = ab ? &dp->octB : &dp->octA;
        BulkOctaveSet *set = &sets[ab];
        double m = mtab[ab];
        set->cnt = on->octcnt > BULK_MAX_OCT ? BULK_MAX_OCT : on->octcnt;
        for (int i = 0; i < set->cnt; i++) {
            const PerlinNoise *p = on->octaves + i;
            BulkOct *bo = &set->oct[i];
            bo->g = NULL;
            double lf = p->lacunarity;
            if (spacing * m * lf > 0.5)
                continue;
            double d0 = bz0 * m * lf + p->a, d1 = bz1 * m * lf + p->a;
            double e0 = bx0 * m * lf + p->b, e1 = bx1 * m * lf + p->b;
            int x0 = (int)floor(fmin(d0, d1)) - 1;
            int x1 = (int)floor(fmax(d0, d1)) + 1;
            int z0 = (int)floor(fmin(e0, e1)) - 1;
            int z1 = (int)floor(fmax(e0, e1)) + 1;
            int gw = x1 - x0 + 1, gh = z1 - z0 + 1;
            if (gw < 2 || gh < 2 || gw > BULK_SPAN_CAP || gh > BULK_SPAN_CAP)
                continue;
            uint8_t *g = (uint8_t *)malloc((size_t)gw * (size_t)gh * 8);
            if (g == NULL)
                continue;
            const uint8_t *idx = p->d;
            uint8_t h3 = (uint8_t)(int)floor(p->c);
            for (int k = 0; k < gh; k++) {
                uint8_t *row = g + (size_t)k * (size_t)gw * 8;
                for (int j = 0; j < gw; j++)
                    bulk_corner_fill_y(row + (size_t)j * 8, idx, x0 + j, z0 + k, h3);
            }
            bo->x0 = x0; bo->z0 = z0; bo->gw = gw; bo->gh = gh;
            bo->g = g;
        }
    }
}

/* The swapped shift DoublePerlin: scalar evaluates sampleDoublePerlin(shift,
 * z, x, 0). A octaves summed, then B octaves, then (vA+vB)*amp. */
static inline double bulk_sample_double_swap(const BulkNoiseCache *bc,
                                             const BiomeNoise *bn,
                                             double x, double z)
{
    const DoublePerlinNoise *dp = &bn->climate[NP_SHIFT];
    const BulkOctaveSet *sa = &bc->shift_swap[0];
    const BulkOctaveSet *sb = &bc->shift_swap[1];
    const double fB = 337.0 / 331.0;
    double vA = 0.0, vB = 0.0;
    int ca = sa->cnt, cb = sb->cnt;
    if (ca > dp->octA.octcnt) ca = dp->octA.octcnt;
    if (cb > dp->octB.octcnt) cb = dp->octB.octcnt;
    for (int i = 0; i < ca; i++) {
        const PerlinNoise *p = dp->octA.octaves + i;
        const BulkOct *bo = &sa->oct[i];
        vA += p->amplitude * (bo->g ? bulk_sample_perlin_swap(bo, p, x, z)
                                    : samplePerlin(p, z * p->lacunarity,
                                                   x * p->lacunarity, 0.0, 0, 0));
    }
    for (int i = 0; i < cb; i++) {
        const PerlinNoise *p = dp->octB.octaves + i;
        const BulkOct *bo = &sb->oct[i];
        double mx = x * fB, mz = z * fB;
        vB += p->amplitude * (bo->g ? bulk_sample_perlin_swap(bo, p, mx, mz)
                                    : samplePerlin(p, mz * p->lacunarity,
                                                   mx * p->lacunarity, 0.0, 0, 0));
    }
    return (vA + vB) * dp->amplitude;
}

/* Precompute the 8 gradient indices for lattice corner (I, K) at y=0.
 * Replicates samplePerlin's idx chain exactly (uint8_t arithmetic wraps). */
static inline void bulk_corner_fill(uint8_t *g, const uint8_t *idx,
                                    int I, int K, uint8_t h2)
{
    uint8_t H1 = (uint8_t)I, H3 = (uint8_t)K;
    uint8_t a1 = (uint8_t)(idx[H1]     + h2);
    uint8_t b1 = (uint8_t)(idx[H1 + 1] + h2);
    uint8_t a2 = (uint8_t)(idx[a1]     + H3);
    uint8_t a3 = (uint8_t)(idx[a1 + 1] + H3);
    uint8_t b2 = (uint8_t)(idx[b1]     + H3);
    uint8_t b3 = (uint8_t)(idx[b1 + 1] + H3);
    g[0] = idx[a2];     /* v4.a -> l1 */
    g[1] = idx[a2 + 1]; /* v4.b -> l5 */
    g[2] = idx[b2];     /* v6.a -> l2 */
    g[3] = idx[b2 + 1]; /* v6.b -> l6 */
    g[4] = idx[a3];     /* v5.a -> l3 */
    g[5] = idx[a3 + 1]; /* v5.b -> l7 */
    g[6] = idx[b3];     /* v7.a -> l4 */
    g[7] = idx[b3 + 1]; /* v7.b -> l8 */
}

/* samplePerlin at y=0 with the corner idx chain precomputed in bo->g.
 * Op-for-op identical to samplePerlin's d2==0 path. */
static inline double bulk_sample_perlin(const BulkOct *bo, const PerlinNoise *p,
                                        double x, double z)
{
    double lf = p->lacunarity;
    double d1 = x * lf + p->a;
    double d3 = z * lf + p->c;
    double i1 = floor(d1), i3 = floor(d3);
    int gx = (int)i1 - bo->x0;
    int gz = (int)i3 - bo->z0;
    if ((unsigned)gx > (unsigned)(bo->gw - 2) ||
        (unsigned)gz > (unsigned)(bo->gh - 2))
        return samplePerlin(p, x * lf, 0.0, z * lf, 0, 0); /* safety fallback */

    double f1 = d1 - i1, f3 = d3 - i3;
    double d2 = p->d2, t2 = p->t2;
    double t1 = f1*f1*f1 * (f1 * (f1*6.0 - 15.0) + 10.0);
    double t3 = f3*f3*f3 * (f3 * (f3*6.0 - 15.0) + 10.0);
    const uint8_t *g = bo->g + ((size_t)gz * (size_t)bo->gw + (size_t)gx) * 8;

    double l1 = bulk_indexed_lerp(g[0], f1,     d2,   f3);
    double l5 = bulk_indexed_lerp(g[1], f1,     d2,   f3 - 1.0);
    double l2 = bulk_indexed_lerp(g[2], f1 - 1.0, d2,   f3);
    double l6 = bulk_indexed_lerp(g[3], f1 - 1.0, d2,   f3 - 1.0);
    double l3 = bulk_indexed_lerp(g[4], f1,     d2-1, f3);
    double l7 = bulk_indexed_lerp(g[5], f1,     d2-1, f3 - 1.0);
    double l4 = bulk_indexed_lerp(g[6], f1 - 1.0, d2-1, f3);
    double l8 = bulk_indexed_lerp(g[7], f1 - 1.0, d2-1, f3 - 1.0);

    l1 = lerp(t1, l1, l2); l3 = lerp(t1, l3, l4);
    l5 = lerp(t1, l5, l6); l7 = lerp(t1, l7, l8);
    l1 = lerp(t2, l1, l3); l5 = lerp(t2, l5, l7);
    return lerp(t3, l1, l5);
}

/* Direct octave eval — mirrors sampleOctave's per-octave body (y=0). */
static inline double bulk_sample_perlin_direct(const PerlinNoise *p, double x, double z)
{
    return samplePerlin(p, x * p->lacunarity, 0.0, z * p->lacunarity, 0, 0);
}

/* Build corner caches for one DoublePerlin parameter over the coordinate
 * rectangle [cx0,cx1]x[cz0,cz1]. spacing = sample distance in the same
 * coords. Octaves are cached only when samples share cells and the span
 * fits; uncached octaves fall back to direct sampling. */
static void bulk_build_param(BulkOctaveSet *sets, const DoublePerlinNoise *dp,
                             double cx0, double cx1, double cz0, double cz1,
                             double spacing)
{
    const double fB = 337.0 / 331.0;
    const double mtab[2] = { 1.0, fB };
    for (int ab = 0; ab < 2; ab++) {
        const OctaveNoise *on = ab ? &dp->octB : &dp->octA;
        BulkOctaveSet *set = &sets[ab];
        double m = mtab[ab];
        set->cnt = on->octcnt > BULK_MAX_OCT ? BULK_MAX_OCT : on->octcnt;
        for (int i = 0; i < set->cnt; i++) {
            const PerlinNoise *p = on->octaves + i;
            BulkOct *bo = &set->oct[i];
            bo->g = NULL;
            double cells_per_sample = spacing * m * p->lacunarity;
            if (cells_per_sample > 0.5)
                continue; /* samples rarely share cells in this octave */
            double lf = p->lacunarity;
            double d0x = cx0 * m * lf + p->a, d1x = cx1 * m * lf + p->a;
            double d0z = cz0 * m * lf + p->c, d1z = cz1 * m * lf + p->c;
            int x0 = (int)floor(fmin(d0x, d1x)) - 1;
            int x1 = (int)floor(fmax(d0x, d1x)) + 1;
            int z0 = (int)floor(fmin(d0z, d1z)) - 1;
            int z1 = (int)floor(fmax(d0z, d1z)) + 1;
            int gw = x1 - x0 + 1, gh = z1 - z0 + 1;
            if (gw < 2 || gh < 2 || gw > BULK_SPAN_CAP || gh > BULK_SPAN_CAP)
                continue;
            uint8_t *g = (uint8_t *)malloc((size_t)gw * (size_t)gh * 8);
            if (g == NULL)
                continue;
            const uint8_t *idx = p->d;
            uint8_t h2 = p->h2;
            for (int k = 0; k < gh; k++) {
                uint8_t *row = g + (size_t)k * (size_t)gw * 8;
                for (int j = 0; j < gw; j++)
                    bulk_corner_fill(row + (size_t)j * 8, idx, x0 + j, z0 + k, h2);
            }
            bo->x0 = x0; bo->z0 = z0; bo->gw = gw; bo->gh = gh;
            bo->g = g;
        }
    }
}

void bulk_cache_free(BulkNoiseCache *bc)
{
    for (int pi = 0; pi < BULK_PARAM_COUNT; pi++)
        for (int ab = 0; ab < 2; ab++)
            for (int i = 0; i < BULK_MAX_OCT; i++) {
                free(bc->set[pi][ab].oct[i].g);
                bc->set[pi][ab].oct[i].g = NULL;
            }
    for (int ab = 0; ab < 2; ab++)
        for (int i = 0; i < BULK_MAX_OCT; i++) {
            free(bc->shift_swap[ab].oct[i].g);
            bc->shift_swap[ab].oct[i].g = NULL;
        }
}

/* Build the shared noise map for a rectangle of chunks. Sample coords are
 * biome-scale (block/4). The shift parameter offsets the other params'
 * coords by up to ~±4, so those get an 8.0 pad; the shift cache itself is
 * built over the union rectangle because pz samples it at (z, x). */
void bulk_cache_build_region(BulkNoiseCache *bc, const BiomeNoise *bn,
                                    int region_chunk_x, int region_chunk_z,
                                    int region_width, int region_height,
                                    int sampling_step)
{
    memset(bc, 0, sizeof(*bc));
    double bx0 = (double)(region_chunk_x * 16) / 4.0;
    double bx1 = (double)((region_chunk_x + region_width) * 16 - 1) / 4.0;
    double bz0 = (double)(region_chunk_z * 16) / 4.0;
    double bz1 = (double)((region_chunk_z + region_height) * 16 - 1) / 4.0;
    double spacing = (double)sampling_step / 4.0;
    if (spacing < 0.25) spacing = 0.25;

    /* shift, normal call: sampled at (x, 0, z) — a plain 2D slice */
    bulk_build_param(bc->set[0], &bn->climate[NP_SHIFT], bx0, bx1, bz0, bz1, spacing);
    /* shift, swapped call: sampled at (z, x, 0) — the 3D-line flavor */
    bulk_build_param_swap(bc->shift_swap, &bn->climate[NP_SHIFT],
                          bx0, bx1, bz0, bz1, spacing);
    /* climate params: sampled at px/pz = coord + shift*4 (± pad) */
    bulk_build_param(bc->set[1], &bn->climate[NP_CONTINENTALNESS], bx0-8, bx1+8, bz0-8, bz1+8, spacing);
    bulk_build_param(bc->set[2], &bn->climate[NP_EROSION],         bx0-8, bx1+8, bz0-8, bz1+8, spacing);
    bulk_build_param(bc->set[3], &bn->climate[NP_WEIRDNESS],       bx0-8, bx1+8, bz0-8, bz1+8, spacing);
    bulk_build_param(bc->set[4], &bn->climate[NP_TEMPERATURE],     bx0-8, bx1+8, bz0-8, bz1+8, spacing);
    bulk_build_param(bc->set[5], &bn->climate[NP_HUMIDITY],        bx0-8, bx1+8, bz0-8, bz1+8, spacing);
}

/* One DoublePerlin evaluation through the cache. Byte-identical to
 * sampleDoublePerlin: A octaves summed, then B octaves, then (vA+vB)*amp —
 * the same association order. */
static inline double bulk_sample_double(const BulkNoiseCache *bc, int param,
                                        const BiomeNoise *bn, int np_idx,
                                        double x, double z)
{
    const DoublePerlinNoise *dp = &bn->climate[np_idx];
    const BulkOctaveSet *sa = &bc->set[param][0];
    const BulkOctaveSet *sb = &bc->set[param][1];
    const double fB = 337.0 / 331.0;
    double vA = 0.0, vB = 0.0;
    int ca = sa->cnt, cb = sb->cnt;
    if (ca > dp->octA.octcnt) ca = dp->octA.octcnt;
    if (cb > dp->octB.octcnt) cb = dp->octB.octcnt;
    for (int i = 0; i < ca; i++) {
        const PerlinNoise *p = dp->octA.octaves + i;
        const BulkOct *bo = &sa->oct[i];
        vA += p->amplitude * (bo->g ? bulk_sample_perlin(bo, p, x, z)
                                    : bulk_sample_perlin_direct(p, x, z));
    }
    for (int i = 0; i < cb; i++) {
        const PerlinNoise *p = dp->octB.octaves + i;
        const BulkOct *bo = &sb->oct[i];
        double bx = x * fB, bz = z * fB;
        vB += p->amplitude * (bo->g ? bulk_sample_perlin(bo, p, bx, bz)
                                    : bulk_sample_perlin_direct(p, bx, bz));
    }
    return (vA + vB) * dp->amplitude;
}

/* Bulk twin of sample_biome_noise_pair — same math, cache-fed octaves. */
int sample_biome_noise_pair_bulk(const BulkNoiseCache *bc, const BiomeNoise *bn,
                                        double x, double z,
                                        int64_t *np_height, int want_biome)
{
    float c, e, w, t, h, d;
    double px = x + bulk_sample_double(bc, 0, bn, NP_SHIFT, x, z) * 4.0;
    double pz = z + bulk_sample_double_swap(bc, bn, x, z) * 4.0;

    c = (float)bulk_sample_double(bc, 1, bn, NP_CONTINENTALNESS, px, pz);
    e = (float)bulk_sample_double(bc, 2, bn, NP_EROSION, px, pz);
    w = (float)bulk_sample_double(bc, 3, bn, NP_WEIRDNESS, px, pz);

    float np_param[] = {
        c, e, -3.0f * (fabsf(fabsf(w) - 0.6666667f) - 0.33333334f), w,
    };
    double off = getSpline(bn->sp, np_param) + 0.015f;
    /* Height depth with y=0 (float, matching the scalar pair sampler). */
    d = (float)(1.0 - 83.0 / 160.0 + off);

    t = (float)bulk_sample_double(bc, 4, bn, NP_TEMPERATURE, px, pz);
    h = (float)bulk_sample_double(bc, 5, bn, NP_HUMIDITY, px, pz);

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

    /* Biome lookup — preserves the scalar path's depth=0 behavior. */
    int64_t np_biome[6];
    np_biome[0] = p_np[0];
    np_biome[1] = p_np[1];
    np_biome[2] = p_np[2];
    np_biome[3] = p_np[3];
    np_biome[4] = 0;
    np_biome[5] = p_np[5];
    return climateToBiome(bn->mc, (const uint64_t *)np_biome, NULL);
}
