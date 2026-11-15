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

#ifndef NULLFRAME_SURFACE_GENERATION_H
#define NULLFRAME_SURFACE_GENERATION_H

#include "generator.h"
#include "core_types.h"
Generator *get_cached_generator(uint64_t seed);

SurfaceNoise *get_cached_surface_noise(void);

EndNoise *get_cached_end_noise(uint64_t seed);

SurfaceNoise *get_cached_end_surface_noise(void);

int myelin_get_terrain_height(long long seed, int dimension_id,
                                  int x, int z);

void myelin_get_surface_chunk_data(long long seed, int dimension_id,
                                       int chunk_x, int chunk_z, int *out_data,
                                       int sampling_step);

void myelin_get_surface_region_data(long long seed, int dimension_id,
                                        int region_chunk_x, int region_chunk_z,
                                        int region_width, int region_height,
                                        int sampling_step, int *out_data);

void myelin_get_surface_region_data_masked(long long seed, int dimension_id,
                                               int region_chunk_x, int region_chunk_z,
                                               int region_width, int region_height,
                                               int sampling_step,
                                               const unsigned char *mask, int *out_data);

void myelin_get_region_surface_data(long long seed, int dimension_id,
                                        int region_chunk_x, int region_chunk_z,
                                        int region_width, int region_height,
                                        int *out_data);

void myelin_get_lod_grid(long long seed, int cell_size_blocks,
                             int min_block_x, int min_block_z,
                             int grid_w, int grid_h,
                             int sampling_step,
                             int *out_heights, int *out_biomes, int *out_types);

#endif
