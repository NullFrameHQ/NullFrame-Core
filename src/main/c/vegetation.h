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

#ifndef NULLFRAME_VEGETATION_H
#define NULLFRAME_VEGETATION_H

#include "vegetation_internal.h"
int generate_chunk_vegetation(uint64_t world_seed, int chunk_x, int chunk_z,
                                     const int *heights, const int *biomes,
                                     const int *surface_types,
                                     int bottom_y, int top_y_exclusive,
                                     VegBuffer vb);

int *myelin_get_vegetation(long long seed,
                                int chunk_x, int chunk_z,
                                const int *heights,        /* 256 ints */
                                const int *biomes,         /* 256 ints */
                                const int *surface_types,  /* 256 ints */
                                int bottom_y,
                                int top_y_exclusive,
                                int *out_int_count);

int *myelin_get_region_vegetation(long long seed, int count,
                                      const int *chunk_xs, const int *chunk_zs,
                                      const int *flat_heights, const int *flat_biomes,
                                      const int *flat_surface_types,
                                      int bottom_y, int top_y_exclusive,
                                      int *out_int_count);

#endif
