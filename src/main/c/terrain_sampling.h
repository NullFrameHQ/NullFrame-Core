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

#ifndef NULLFRAME_TERRAIN_SAMPLING_H
#define NULLFRAME_TERRAIN_SAMPLING_H

#include "terrain_noise_cache.h"
extern volatile int g_correction_amp;
extern volatile int g_biome_step;

int sample_biome_noise_double(const BiomeNoise *bn, int64_t *np,
                                      double x, int y, double z,
                                      uint64_t *dat, uint32_t sample_flags);

int sample_biome_noise_pair(const BiomeNoise *bn, double x, double z,
                                   int64_t *np_height, int want_biome);

int map_overworld_height_sample(Generator *g,
                                       const SurfaceNoise *sn,
                                       int quarter_x,
                                       int quarter_z);

void apply_staircase_shaping(float *out_heights, int base_x, int base_z,
                                    int sampling_step,
                                    float h00, float h10, float h01, float h11,
                                    int b00, int b10, int b01, int b11);

void corner_sample_cached(const Generator *g, int qx, int qz,
                                 const BulkNoiseCache *bc,
                                 float *out_h, int *out_b);

void map_approx_height_fine(float *out_heights, int *out_biomes,
                                     const Generator *g,
                                     int chunk_x, int chunk_z,
                                     int sampling_step,
                                     const BulkNoiseCache *bc);

#endif
