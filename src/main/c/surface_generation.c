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
#include "surface_generation.h"
#include "surface_classification.h"
#include "terrain_sampling.h"
#include "terrain_math.h"

static TLS_STORAGE Generator *tls_g = NULL;
static TLS_STORAGE SurfaceNoise *tls_sn = NULL;
static TLS_STORAGE uint64_t tls_seed = (uint64_t)-1;

Generator *get_cached_generator(uint64_t seed) {
    if (tls_g == NULL) {
        tls_g = (Generator *)malloc(sizeof(Generator));
        if (tls_g == NULL) return NULL;
        memset(tls_g, 0, sizeof(Generator));
    }
    if (tls_sn == NULL) {
        tls_sn = (SurfaceNoise *)malloc(sizeof(SurfaceNoise));
        if (tls_sn == NULL) return NULL;
        memset(tls_sn, 0, sizeof(SurfaceNoise));
    }
    if (tls_seed != seed) {
        setupGenerator(tls_g, MC_1_21_1, 0);
        applySeed(tls_g, DIM_OVERWORLD, seed);
        initSurfaceNoise(tls_sn, DIM_OVERWORLD, seed);
        tls_seed = seed;
    }
    return tls_g;
}

SurfaceNoise *get_cached_surface_noise(void) {
    return tls_sn;
}

/* Reuse End noise state per thread, rebuilding it when the seed changes. */
static TLS_STORAGE EndNoise *tls_en = NULL;
static TLS_STORAGE SurfaceNoise *tls_end_sn = NULL;
static TLS_STORAGE uint64_t tls_end_seed = (uint64_t)-1;

EndNoise *get_cached_end_noise(uint64_t seed) {
    if (tls_en == NULL) {
        tls_en = (EndNoise *)malloc(sizeof(EndNoise));
        if (tls_en == NULL) return NULL;
        memset(tls_en, 0, sizeof(EndNoise));
    }
    if (tls_end_sn == NULL) {
        tls_end_sn = (SurfaceNoise *)malloc(sizeof(SurfaceNoise));
        if (tls_end_sn == NULL) return NULL;
        memset(tls_end_sn, 0, sizeof(SurfaceNoise));
    }
    if (tls_end_seed != seed) {
        setEndSeed(tls_en, MC_1_21_1, seed);
        initSurfaceNoise(tls_end_sn, DIM_END, seed);
        tls_end_seed = seed;
    }
    return tls_en;
}

SurfaceNoise *get_cached_end_surface_noise(void) {
    return tls_end_sn;
}


/* Sample each coarse-grid biome at its estimated surface height using sampleBiomeNoise. */
static void fill_surface_biome_grid_accurate(Generator *g,
                                             const float *coarse_heights,
                                             int coarse_origin_x,
                                             int coarse_origin_z,
                                             int coarse_width,
                                             int coarse_height_grid,
                                             int *out_biomes) {
    if (g == NULL || coarse_heights == NULL || out_biomes == NULL) return;

    for (int coarse_z = 0; coarse_z < coarse_height_grid; coarse_z++) {
        for (int coarse_x = 0; coarse_x < coarse_width; coarse_x++) {
            int idx = coarse_z * coarse_width + coarse_x;
            int approx_surface_y = round_to_int(coarse_heights[idx]);
            int quarter_y = floor_div_4(approx_surface_y);
            out_biomes[idx] = sampleBiomeNoise(&g->bn, NULL,
                                                coarse_origin_x + coarse_x,
                                                quarter_y,
                                                coarse_origin_z + coarse_z,
                                                NULL, 0);
        }
    }
}


/* Fill all three chunk arrays with constant fallback values. */

static void fill_default_surface_chunk_data(int *out_data, int height, int surface_type, int biome_id) {
    if (out_data == NULL) return;
    int *out_heights = out_data;
    int *out_types = out_data + CHUNK_COLUMN_COUNT;
    int *out_biomes = out_data + (CHUNK_COLUMN_COUNT * 2);
    for (int i = 0; i < CHUNK_COLUMN_COUNT; i++) {
        out_heights[i] = height;
        out_types[i] = surface_type;
        out_biomes[i] = biome_id;
    }
}

static void pack_region_chunk_surface_data(const float *coarse_heights,
                                           const int *coarse_biomes,
                                           int coarse_width,
                                           int chunk_offset_x,
                                           int chunk_offset_z,
                                           int *out_data) {
    if (coarse_heights == NULL || coarse_biomes == NULL || out_data == NULL) return;

    int *out_heights = out_data;
    int *out_types = out_data + CHUNK_COLUMN_COUNT;
    int *out_biomes = out_data + (CHUNK_COLUMN_COUNT * 2);
    int base_x = chunk_offset_x * 4;
    int base_z = chunk_offset_z * 4;

    for (int local_z = 0; local_z < 16; local_z++) {
        int cell_z = local_z >> 2;
        float tz = (local_z & 3) * 0.25f;

        for (int local_x = 0; local_x < 16; local_x++) {
            int cell_x = local_x >> 2;
            float tx = (local_x & 3) * 0.25f;

            int coarse_x = base_x + cell_x;
            int coarse_z = base_z + cell_z;

            float h00 = coarse_heights[coarse_z * coarse_width + coarse_x];
            float h10 = coarse_heights[coarse_z * coarse_width + (coarse_x + 1)];
            float h01 = coarse_heights[(coarse_z + 1) * coarse_width + coarse_x];
            float h11 = coarse_heights[(coarse_z + 1) * coarse_width + (coarse_x + 1)];
            int b00 = coarse_biomes[coarse_z * coarse_width + coarse_x];
            int b10 = coarse_biomes[coarse_z * coarse_width + (coarse_x + 1)];
            int b01 = coarse_biomes[(coarse_z + 1) * coarse_width + coarse_x];
            int b11 = coarse_biomes[(coarse_z + 1) * coarse_width + (coarse_x + 1)];
            int surface_y = round_to_int(bilerp(h00, h10, h01, h11, tx, tz));
            int biome_id = interpolate_biome_id_discrete(b00, b10, b01, b11, tx, tz);
            int idx = local_z * 16 + local_x;

            out_heights[idx] = surface_y;
            out_types[idx] = classify_surface_type(biome_id, surface_y);
            out_biomes[idx] = biome_id;
        }
    }
}

/* Single-position height query, with a separate End path. */

int myelin_get_terrain_height(long long seed, int dimension_id,
                                  int x, int z) {
    if (dimension_id == CB_DIM_END) {
        return getEndSurfaceHeight(MC_1_21_1, (uint64_t) seed, x, z);
    }
    if (dimension_id != CB_DIM_OVERWORLD) return 64;

    Generator *g = get_cached_generator((uint64_t) seed);
    SurfaceNoise *sn = get_cached_surface_noise();
    if (g == NULL || sn == NULL) return 64;

    return map_overworld_height_sample(g, sn, floor_div_4(x), floor_div_4(z));
}


/* Generate one chunk using the requested terrain sampling step. */

void myelin_get_surface_chunk_data(long long seed, int dimension_id,
                                       int chunk_x, int chunk_z, int *out_data,
                                       int sampling_step) {
    if (out_data == NULL) return;

    int *out_heights = out_data;
    int *out_types = out_data + CHUNK_COLUMN_COUNT;
    int *out_biomes = out_data + (CHUNK_COLUMN_COUNT * 2);

    if (dimension_id == CB_DIM_END) {
        EndNoise *en = get_cached_end_noise((uint64_t) seed);
        SurfaceNoise *end_sn = get_cached_end_surface_noise();
        if (en != NULL && end_sn != NULL) {
            /* Batch version — one call for all 256 columns (TLS cached) */
            float end_heights[256];
            if (mapEndSurfaceHeight(end_heights, en, end_sn,
                                     chunk_x * 16, chunk_z * 16, 16, 16, 1, 0) == 0) {
                for (int i = 0; i < CHUNK_COLUMN_COUNT; i++) {
                    out_heights[i] = round_to_int(end_heights[i]);
                    out_types[i] = SURFACE_STONE;
                    out_biomes[i] = the_end;
                }
                return;
            }
        }
        /* Fallback: per-point (slow, but works if TLS failed) */
        for (int lz = 0; lz < 16; lz++) {
            for (int lx = 0; lx < 16; lx++) {
                int wx = chunk_x * 16 + lx;
                int wz = chunk_z * 16 + lz;
                int h = getEndSurfaceHeight(MC_1_21_1, (uint64_t) seed, wx, wz);
                int idx = lz * 16 + lx;
                out_heights[idx] = h;
                out_types[idx] = SURFACE_STONE;
                out_biomes[idx] = the_end;
            }
        }
        return;
    }

    if (dimension_id != CB_DIM_OVERWORLD) {
        for (int i = 0; i < CHUNK_COLUMN_COUNT; i++) {
            out_heights[i] = 64;
            out_types[i] = SURFACE_GRASS;
            out_biomes[i] = DEFAULT_PLAINS_BIOME_ID;
        }
        return;
    }

    Generator *g = get_cached_generator((uint64_t) seed);
    if (g == NULL) {
        for (int i = 0; i < CHUNK_COLUMN_COUNT; i++) {
            out_heights[i] = 64;
            out_types[i] = SURFACE_GRASS;
            out_biomes[i] = DEFAULT_PLAINS_BIOME_ID;
        }
        return;
    }

    /* Sample the chunk heights and biomes, then classify each surface column. */
    float fine_heights[256];
    int fine_biomes[256];
    map_approx_height_fine(fine_heights, fine_biomes, g, chunk_x, chunk_z, sampling_step, NULL);

    ClassifyMemo memo;
    classify_memo_init(&memo);
    for (int i = 0; i < CHUNK_COLUMN_COUNT; i++) {
        int surface_y = round_to_int(fine_heights[i]);
        int biome_id = fine_biomes[i];
        out_heights[i] = surface_y;
        out_types[i] = classify_memo_get(&memo, biome_id);
        out_biomes[i] = biome_id;
    }
}

/*
 * Generate a rectangle in row-major chunk order. Selected chunks share a noise cache,
 * while each chunk uses the same sampling routine as the single-chunk path.
 */

static void region_data_impl(long long seed, int dimension_id,
                             int region_chunk_x, int region_chunk_z,
                             int region_width, int region_height,
                             int sampling_step,
                             const unsigned char *mask, int *out_data) {
    if (out_data == NULL || region_width <= 0 || region_height <= 0) return;

    int chunk_count = region_width * region_height;
    if (dimension_id == CB_DIM_END) {
        /* Mirror the per-chunk path: real End terrain via EndNoise. */
        for (int rz = 0; rz < region_height; rz++) {
            for (int rx = 0; rx < region_width; rx++) {
                int idx = rz * region_width + rx;
                if (mask != NULL && mask[idx] == 0) continue;
                int *chunk_out = out_data + idx * PACKED_CHUNK_INTS;
                myelin_get_surface_chunk_data(seed, dimension_id,
                        region_chunk_x + rx, region_chunk_z + rz, chunk_out, sampling_step);
            }
        }
        return;
    }
    if (dimension_id != CB_DIM_OVERWORLD) {
        for (int chunk_index = 0; chunk_index < chunk_count; chunk_index++) {
            if (mask != NULL && mask[chunk_index] == 0) continue;
            fill_default_surface_chunk_data(
                    out_data + chunk_index * PACKED_CHUNK_INTS,
                    64, SURFACE_GRASS, DEFAULT_PLAINS_BIOME_ID);
        }
        return;
    }

    Generator *g = get_cached_generator((uint64_t) seed);
    if (g == NULL) {
        for (int chunk_index = 0; chunk_index < chunk_count; chunk_index++) {
            if (mask != NULL && mask[chunk_index] == 0) continue;
            fill_default_surface_chunk_data(
                    out_data + chunk_index * PACKED_CHUNK_INTS,
                    64, SURFACE_GRASS, DEFAULT_PLAINS_BIOME_ID);
        }
        return;
    }

    float fine_heights[256];
    int fine_biomes[256];

    /*
     * Build one gradient cache for the region. Sampling falls back to direct noise
     * evaluation outside the cached bounds.
     */
    BulkNoiseCache bulk_bc;
    bulk_cache_build_region(&bulk_bc, &g->bn, region_chunk_x, region_chunk_z,
                            region_width, region_height, sampling_step);

    for (int rz = 0; rz < region_height; rz++) {
        for (int rx = 0; rx < region_width; rx++) {
            int idx = rz * region_width + rx;
            if (mask != NULL && mask[idx] == 0) continue;
            int *chunk_out = out_data + idx * PACKED_CHUNK_INTS;
            map_approx_height_fine(fine_heights, fine_biomes, g,
                                   region_chunk_x + rx, region_chunk_z + rz,
                                   sampling_step, &bulk_bc);
            ClassifyMemo memo;
            classify_memo_init(&memo);
            for (int i = 0; i < CHUNK_COLUMN_COUNT; i++) {
                int surface_y = round_to_int(fine_heights[i]);
                int biome_id = fine_biomes[i];
                chunk_out[i] = surface_y;
                chunk_out[CHUNK_COLUMN_COUNT + i] = classify_memo_get(&memo, biome_id);
                chunk_out[CHUNK_COLUMN_COUNT * 2 + i] = biome_id;
            }
        }
    }

    bulk_cache_free(&bulk_bc);
}

void myelin_get_surface_region_data(long long seed, int dimension_id,
                                        int region_chunk_x, int region_chunk_z,
                                        int region_width, int region_height,
                                        int sampling_step, int *out_data) {
    region_data_impl(seed, dimension_id, region_chunk_x, region_chunk_z,
                     region_width, region_height, sampling_step, NULL, out_data);
}

/* Same, but ONLY computes chunks where mask[idx] != 0 — the rest of the
 * rectangle (already cached / pending) is skipped, so the C engine never
 * re-generates what the server already has. Mask is row-major,
 * region_width × region_height; NULL = compute everything. */
void myelin_get_surface_region_data_masked(long long seed, int dimension_id,
                                               int region_chunk_x, int region_chunk_z,
                                               int region_width, int region_height,
                                               int sampling_step,
                                               const unsigned char *mask, int *out_data) {
    region_data_impl(seed, dimension_id, region_chunk_x, region_chunk_z,
                     region_width, region_height, sampling_step, mask, out_data);
}

/* Legacy region path: sample a coarse height grid, then interpolate each chunk. */

void myelin_get_region_surface_data(long long seed, int dimension_id,
                                        int region_chunk_x, int region_chunk_z,
                                        int region_width, int region_height,
                                        int *out_data) {
    if (out_data == NULL || region_width <= 0 || region_height <= 0) return;

    int chunk_count = region_width * region_height;
    if (dimension_id != CB_DIM_OVERWORLD) {
        for (int chunk_index = 0; chunk_index < chunk_count; chunk_index++) {
            fill_default_surface_chunk_data(
                    out_data + chunk_index * PACKED_CHUNK_INTS,
                    64, SURFACE_GRASS, DEFAULT_PLAINS_BIOME_ID);
        }
        return;
    }

    Generator *g = get_cached_generator((uint64_t) seed);
    SurfaceNoise *sn = get_cached_surface_noise();
    if (g == NULL || sn == NULL) {
        for (int chunk_index = 0; chunk_index < chunk_count; chunk_index++) {
            fill_default_surface_chunk_data(
                    out_data + chunk_index * PACKED_CHUNK_INTS,
                    64, SURFACE_GRASS, DEFAULT_PLAINS_BIOME_ID);
        }
        return;
    }

    int coarse_width = region_width * 4 + 1;
    int coarse_height = region_height * 4 + 1;
    size_t coarse_count = (size_t) coarse_width * (size_t) coarse_height;
    float *coarse_heights = (float *) malloc(sizeof(float) * coarse_count);
    int *coarse_biomes = (int *) malloc(sizeof(int) * coarse_count);

    if (coarse_heights == NULL || coarse_biomes == NULL) {
        free(coarse_heights);
        free(coarse_biomes);
        for (int chunk_index = 0; chunk_index < chunk_count; chunk_index++) {
            fill_default_surface_chunk_data(
                    out_data + chunk_index * PACKED_CHUNK_INTS,
                    64, SURFACE_GRASS, DEFAULT_PLAINS_BIOME_ID);
        }
        return;
    }

    if (mapApproxHeight(coarse_heights, NULL, g, sn,
                        region_chunk_x * 4, region_chunk_z * 4,
                        coarse_width, coarse_height) != 0) {
        free(coarse_heights);
        free(coarse_biomes);
        for (int chunk_index = 0; chunk_index < chunk_count; chunk_index++) {
            fill_default_surface_chunk_data(
                    out_data + chunk_index * PACKED_CHUNK_INTS,
                    64, SURFACE_GRASS, DEFAULT_PLAINS_BIOME_ID);
        }
        return;
    }

    fill_surface_biome_grid_accurate(g, coarse_heights,
                                     region_chunk_x * 4, region_chunk_z * 4,
                                     coarse_width, coarse_height, coarse_biomes);

    int chunk_index = 0;
    for (int region_z = 0; region_z < region_height; region_z++) {
        for (int region_x = 0; region_x < region_width; region_x++) {
            pack_region_chunk_surface_data(
                    coarse_heights, coarse_biomes, coarse_width,
                    region_x, region_z,
                    out_data + chunk_index * PACKED_CHUNK_INTS);
            chunk_index++;
        }
    }

    free(coarse_heights);
    free(coarse_biomes);
}

/*
 * Sample a grid of columns at the requested block spacing.
 * Uses cached quarter-coordinate corner samples and optional detail correction.
 * It does not apply the mountain staircase pass used by chunk generation.
 */
void myelin_get_lod_grid(long long seed, int cell_size_blocks,
                             int min_block_x, int min_block_z,
                             int grid_w, int grid_h,
                             int sampling_step,
                             int *out_heights, int *out_biomes, int *out_types) {
    if (out_heights == NULL || out_biomes == NULL || out_types == NULL) return;
    if (cell_size_blocks < 1) cell_size_blocks = 1;
    int count = grid_w * grid_h;
    Generator *g = get_cached_generator((uint64_t) seed);
    if (g == NULL) {
        for (int i = 0; i < count; i++) {
            out_heights[i] = 64;
            out_biomes[i] = DEFAULT_PLAINS_BIOME_ID;
            out_types[i] = SURFACE_GRASS;
        }
        return;
    }
    /* Correction — same gate + amplitude as map_approx_height_fine. */
    int corr_amp = g_correction_amp;
    float eff_corr_amp = 0.0f;
    if (corr_amp > 0 && sampling_step >= 16) {
        eff_corr_amp = (float) (corr_amp * (sampling_step >= 32 ? 3 : 2)) / 4.0f;
    }
    ClassifyMemo memo;
    classify_memo_init(&memo);
    for (int gz = 0; gz < grid_h; gz++) {
        for (int gx = 0; gx < grid_w; gx++) {
            int bx = min_block_x + gx * cell_size_blocks;
            int bz = min_block_z + gz * cell_size_blocks;
            float h;
            int b;
            corner_sample_cached(g, floor_div_4(bx), floor_div_4(bz), NULL, &h, &b);
            if (eff_corr_amp > 0.0f) {
                /* Quintic-faded 16-block lattice — identical to the block path. */
                int gx16 = floor_div_world(bx, 16) * 16;
                int gz16 = floor_div_world(bz, 16) * 16;
                float fx = (float) (bx - gx16) / 16.0f;
                float fz = (float) (bz - gz16) / 16.0f;
                float n = bilerp(value_noise_hash(gx16, gz16),
                                 value_noise_hash(gx16 + 16, gz16),
                                 value_noise_hash(gx16, gz16 + 16),
                                 value_noise_hash(gx16 + 16, gz16 + 16),
                                 quintic_fade(fx), quintic_fade(fz));
                h += (n * 2.0f - 1.0f) * eff_corr_amp;
            }
            int idx = gz * grid_w + gx;
            out_heights[idx] = round_to_int(h);
            out_biomes[idx] = b;
            out_types[idx] = classify_memo_get(&memo, b);
        }
    }
}
