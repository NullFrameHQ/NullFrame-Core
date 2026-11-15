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

/*
 * Terrain quad format and distance-tiered baking API.
 * Vertices contain world coordinates, UVs, ABGR colour, texture ID and flags.
 * Consumers map the texture IDs to their own rendering format.
 *
 * Cells are aligned to world coordinates and selected by distance from the centre.
 * The implementation caps cells at 16 blocks. One-block cells can merge equal top
 * faces and emit exposed walls; coarser cells do not have transition skirts.
 */
#ifndef QUAD_BAKER_H
#define QUAD_BAKER_H

#include <stdint.h>

#define QB_MAX_TIERS 8
#define QB_SEA_LEVEL 63

/* Per-vertex: 28 bytes. All float/int fields are little-endian on x86. */
typedef struct {
    float x, y, z;     /* world block coords */
    float u, v;        /* synthetic atlas coordinates */
    uint32_t color;    /* ABGR */
    uint8_t texture;   /* atlas tile index (synthetic atlas for the .exe) */
    uint8_t flags;     /* QB_VFLAG_* */
    uint16_t _pad;     /* alignment */
} QbVertex;

/* 4 vertices per quad = 112 bytes. */
typedef struct {
    QbVertex v[4];
} QbQuad;

/* One ring: cells of `cell_blocks` size cover Chebyshev distance band
 * [prev_tier.max_dist, max_dist). `cell_blocks` must be a power of 2 and
 * the table must be sorted by max_dist ascending. */
typedef struct {
    int cell_blocks;
    int max_dist;
} QbTier;

enum {
    QB_VFLAG_TOP   = 1,
    QB_VFLAG_SKIRT = 2,
};

/* Synthetic texture IDs. Consumers must map these IDs to their own textures. */
enum {
    QB_TILE_GRASS  = 0,
    QB_TILE_SAND   = 1,
    QB_TILE_STONE  = 2,
    QB_TILE_SNOW   = 3,
    QB_TILE_WATER  = 4,
    QB_TILE_LEAVES = 5,
    QB_TILE_DIRT   = 6,
    QB_TILE_LOG    = 7,
    QB_TILE_GRASS_SIDE = 8,
};

/*
 * Bake a region of region_w by region_h chunks into quads.
 * The three input arrays each contain 256 values per chunk, in row-major chunk order.
 * Within each chunk, column (x,z) is at z*16+x. Heights are world block Y coordinates.
 * origin_x/z and center_x/z are world block coordinates; tiers select distance bands.
 *
 * out holds capacity quad records. out_count receives the emitted count on success;
 * out_tier_counts has one entry per tier. Size capacity for the selected terrain and tiers.
 * Returns 0 on success, -1 for invalid arguments or -2 when output capacity is exhausted.
 */
int qb_bake_region(const int *heights, const int *types, const int *biomes,
                   int region_w, int region_h,
                   int origin_x, int origin_z,
                   int center_x, int center_z,
                   const QbTier *tiers, int tier_count,
                   QbQuad *out, int capacity,
                   int *out_count, int *out_tier_counts);

/* Same baker for the native generator's chunk-interleaved layout:
 * [heights(256), types(256), biomes(256)] per chunk. */
int qb_bake_region_interleaved(const int *region,
                               int region_w, int region_h,
                               int origin_x, int origin_z,
                               int center_x, int center_z,
                               const QbTier *tiers, int tier_count,
                               QbQuad *out, int capacity,
                               int *out_count, int *out_tier_counts);

#endif /* QUAD_BAKER_H */
