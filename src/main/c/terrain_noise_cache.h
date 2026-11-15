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

#ifndef NULLFRAME_TERRAIN_NOISE_CACHE_H
#define NULLFRAME_TERRAIN_NOISE_CACHE_H

#include "generator.h"
#include "core_types.h"
#define BULK_PARAM_COUNT 6  /* shift, c, e, w, temperature, humidity */
#define BULK_MAX_OCT     32
#define BULK_SPAN_CAP    2048 /* max lattice corners per axis worth caching */

typedef struct {
    int      gw, gh;   /* corner grid dimensions (>= 2) */
    int      x0, z0;   /* lattice coords of corner (0,0) */
    uint8_t *g;        /* gw*gh*8 gradient indices; NULL = octave not cached */
} BulkOct;

typedef struct {
    int     cnt;
    BulkOct oct[BULK_MAX_OCT];
} BulkOctaveSet;

typedef struct {
    BulkOctaveSet set[BULK_PARAM_COUNT][2]; /* [param][octA/octB] */
    BulkOctaveSet shift_swap[2]; /* 3D-line flavor for the swapped shift call */
} BulkNoiseCache;


void bulk_cache_free(BulkNoiseCache *bc);

void bulk_cache_build_region(BulkNoiseCache *bc, const BiomeNoise *bn,
                                    int region_chunk_x, int region_chunk_z,
                                    int region_width, int region_height,
                                    int sampling_step);

int sample_biome_noise_pair_bulk(const BulkNoiseCache *bc, const BiomeNoise *bn,
                                        double x, double z,
                                        int64_t *np_height, int want_biome);

#endif
