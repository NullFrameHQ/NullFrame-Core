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

/* Terrain quad generation. Input comes from surface arrays; JNI handling is separate. */
#include <string.h>
#include <stdlib.h>
#include "quad_baker.h"
#include "biomes.h"   /* biome id constants (cubiomes) */

#define QB_ATLAS_COLS 4
#define QB_ATLAS_ROWS 2
/* Cap cells at 16 blocks because larger cells do not have transition stitching. */
#define QB_SAFE_CELL_BLOCKS 16

/* Surface type IDs must match enum SurfaceType in core_types.h. */
enum {
    QB_SURF_GRASS = 0,
    QB_SURF_SAND  = 1,
    QB_SURF_STONE = 2,
    QB_SURF_SNOW  = 3,
    QB_SURF_WATER = 4,
};

/* helpers */

static int qb_abs(int x) { return x < 0 ? -x : x; }
static int qb_max(int a, int b) { return a > b ? a : b; }
static int qb_min(int a, int b) { return a < b ? a : b; }

/* Floor division by a positive power of two. */
static int qb_floor_div_pow2(int a, int b) {
    if (a >= 0) return a / b;
    return -((-a + b - 1) / b);
}

static int qb_chebyshev(int dx, int dz) {
    int ax = qb_abs(dx), az = qb_abs(dz);
    return ax > az ? ax : az;
}

/* Map a texture ID to the UV centre of its synthetic atlas tile. */
static void qb_tile_uv(int tile, float *u, float *v) {
    int col = tile % QB_ATLAS_COLS;
    int row = tile / QB_ATLAS_COLS;
    *u = ((float)col + 0.5f) / (float)QB_ATLAS_COLS;
    *v = ((float)row + 0.5f) / (float)QB_ATLAS_ROWS;
}

/* ABGR base colors for the synthetic atlas (approximate biome tints). */
static uint32_t qb_tile_color(int tile, int dark) {
    uint32_t c;
    switch (tile) {
        case QB_TILE_GRASS:  c = 0xFF50AF4Cu; break;  /* #4CAF50 */
        case QB_TILE_SAND:   c = 0xFFC2B280u; break;  /* #80B2C2 */
        case QB_TILE_STONE:  c = 0xFF9E9E9Eu; break;
        case QB_TILE_SNOW:   c = 0xFFF5F5F5u; break;
        case QB_TILE_WATER:  c = 0xFFE4763Fu; break;  /* #3F76E4 */
        case QB_TILE_LEAVES: c = 0xFF327D2Eu; break;  /* #2E7D32 */
        case QB_TILE_GRASS_SIDE: c = 0xFF3A8B3Au; break;
        case QB_TILE_DIRT:   c = 0xFF485579u; break;  /* #795548 */
        default:             c = 0xFF37405Du; break;  /* #5D4037 log */
    }
    if (dark) {
        uint32_t r = (c & 0xFFu) * 170 / 255;
        uint32_t g = ((c >> 8) & 0xFFu) * 170 / 255;
        uint32_t b = ((c >> 16) & 0xFFu) * 170 / 255;
        c = (c & 0xFF000000u) | (b << 16) | (g << 8) | r;
    }
    return c;
}

/* Is this biome a forest/jungle/taiga (grass surfaces read as leaves)? */
static int qb_leafy_biome(int biome_id) {
    switch (biome_id) {
        case dark_forest: case 157:   /* dark_forest_hills */
        case jungle: case jungle_hills: case jungle_edge:
        case bamboo_jungle: case bamboo_jungle_hills:
        case forest: case wooded_hills: case flower_forest:
        case giant_tree_taiga: case giant_tree_taiga_hills:
        case 160: case 161:           /* giant_spruce_taiga */
        case birch_forest: case birch_forest_hills:
        case 155: case 156:           /* tall_birch_forest */
        case taiga: case taiga_hills:
        case snowy_taiga: case snowy_taiga_hills: case 159:
        case savanna: case savanna_plateau:
        case shattered_savanna: case 164:
        case swamp: case swamp_hills: case mangrove_swamp:
            return 1;
        default:
            return 0;
    }
}

/* Surface type + biome -> atlas tile (the "representative texture" pick). */
static int qb_tile_for(int surface, int biome_id) {
    switch (surface) {
        case QB_SURF_SAND:  return QB_TILE_SAND;
        case QB_SURF_STONE: return QB_TILE_STONE;
        case QB_SURF_SNOW:  return QB_TILE_SNOW;
        case QB_SURF_WATER: return QB_TILE_WATER;
        /* Choose the terrain material from its surface classification; this does not generate trees. */
        default:            return QB_TILE_GRASS;
    }
}

/* Region sample helpers — chunk-major layout, 256 columns per chunk. */
static int qb_region_value(const int *arr, int rw, int lx, int lz, int chunk_stride) {
    int cx = lx >> 4, cz = lz >> 4;
    int x = lx & 15, z = lz & 15;
    return arr[(cz * rw + cx) * chunk_stride + z * 16 + x];
}

static void qb_set_vertex(QbVertex *v, float x, float y, float z,
                          int tile, int dark, int flags) {
    v->x = x; v->y = y; v->z = z;
    qb_tile_uv(tile, &v->u, &v->v);
    v->color = qb_tile_color(tile, dark);
    v->texture = (uint8_t)tile;
    v->flags = (uint8_t)flags;
    v->_pad = 0;
}

/* Returns the exact flat top key used by a 1x1 heightfield column.  The
 * greedy pass only joins columns with this same key; it never smooths or
 * changes the Cubiomes silhouette. */
static int qb_s1_top_key(const int *heights, const int *types, const int *biomes,
                         int region_w, int bw, int bh, int origin_x, int origin_z,
                         int center_x, int center_z, int min_dist, int max_dist,
                         int wx, int wz, int chunk_stride, int *out_y, int *out_tile) {
    int lx = wx - origin_x, lz = wz - origin_z;
    if (lx < 0 || lz < 0 || lx >= bw || lz >= bh) return 0;
    int dist = qb_chebyshev(wx - center_x, wz - center_z);
    if (dist < min_dist || dist >= max_dist) return 0;
    int h = qb_region_value(heights, region_w, lx, lz, chunk_stride);
    *out_y = h < QB_SEA_LEVEL ? QB_SEA_LEVEL : h;
    *out_tile = h <= QB_SEA_LEVEL ? QB_TILE_WATER :
            qb_tile_for(qb_region_value(types, region_w, lx, lz, chunk_stride),
                        qb_region_value(biomes, region_w, lx, lz, chunk_stride));
    return 1;
}

/* Emits a block-aligned exposed wall. The top block gets grass-side (when
 * applicable), the next three get dirt, and deeper material becomes stone.
 * Each strip is deliberately unmerged for visual correctness. */
static int qb_emit_wall(QbQuad *out, int capacity, int *count, int *tier_count,
                        float x0, float z0, float x1, float z1,
                        int high, int low, int surface) {
    for (int y = high; y > low;) {
        int tile, bottom;
        if (surface == QB_SURF_GRASS && y == high) { tile = QB_TILE_GRASS_SIDE; bottom = y - 1; }
        else if (surface == QB_SURF_STONE) { tile = QB_TILE_STONE; bottom = low; }
        else if (high - y < 4) { tile = QB_TILE_DIRT; bottom = qb_max(low, y - (4 - (high - y))); }
        else { tile = QB_TILE_STONE; bottom = low; }
        if (*count >= capacity) return -2;
        QbQuad *q = &out[(*count)++];
        qb_set_vertex(&q->v[0], x0, (float)y, z0, tile, 1, QB_VFLAG_SKIRT);
        qb_set_vertex(&q->v[1], x1, (float)y, z1, tile, 1, QB_VFLAG_SKIRT);
        qb_set_vertex(&q->v[2], x1, (float)bottom, z1, tile, 1, QB_VFLAG_SKIRT);
        qb_set_vertex(&q->v[3], x0, (float)bottom, z0, tile, 1, QB_VFLAG_SKIRT);
        (*tier_count)++;
        y = bottom;
    }
    return 0;
}

/* qb_bake_region */
static int qb_bake_region_layout(const int *heights, const int *types, const int *biomes,
                   int region_w, int region_h,
                   int origin_x, int origin_z,
                   int center_x, int center_z,
                   const QbTier *tiers, int tier_count,
                   QbQuad *out, int capacity,
                   int *out_count, int *out_tier_counts, int chunk_stride) {
    if (!heights || !types || !biomes || !out || !out_count ||
        region_w <= 0 || region_h <= 0 || tier_count < 1 || tier_count > QB_MAX_TIERS) {
        return -1;
    }
    const int bw = region_w * 16;      /* region size in blocks */
    const int bh = region_h * 16;
    const int sea = QB_SEA_LEVEL;
    int count = 0;
    for (int t = 0; t < tier_count; t++) out_tier_counts[t] = 0;

    int prev_max = 0;
    for (int t = 0; t < tier_count; t++) {
        const int requested_s = tiers[t].cell_blocks;
        const int S = requested_s > QB_SAFE_CELL_BLOCKS ? QB_SAFE_CELL_BLOCKS : requested_s;
        const int maxd = tiers[t].max_dist;
        if (requested_s <= 0 || S <= 0 || maxd <= prev_max) { prev_max = maxd; continue; }

        /* World-anchored cell grid: cells start at multiples of S. Region
         * corner in world coords, rounded down to the S grid. */
        int gx0 = qb_floor_div_pow2(origin_x, S) * S;
        int gz0 = qb_floor_div_pow2(origin_z, S) * S;
        int gx1 = qb_floor_div_pow2(origin_x + bw - 1, S) * S;
        int gz1 = qb_floor_div_pow2(origin_z + bh - 1, S) * S;
        /* Track emitted one-block tops so equal neighbours can be merged without duplicate faces. */
        unsigned char *s1_seen = S == 1 ? (unsigned char *)calloc((size_t)bw * (size_t)bh, 1) : NULL;

        for (int wz = gz0; wz <= gz1; wz += S) {
            for (int wx = gx0; wx <= gx1; wx += S) {
                int ccx = wx + S / 2, ccz = wz + S / 2;
                int dist = qb_chebyshev(ccx - center_x, ccz - center_z);
                if (dist < prev_max || dist >= maxd) continue;

                /* Region-local cell bounds (world -> local). Corner samples
                 * come from the BOUNDARY columns (lx0+S, lz0+S), so adjacent
                 * cells read the SAME column on their shared edge — no seams.
                 * The boundary column only exceeds the region at the outer
                 * edge, where it clamps to bw-1/bh-1. */
                int lx0 = wx - origin_x, lz0 = wz - origin_z;
                if (lx0 >= bw || lz0 >= bh || lx0 + S <= 0 || lz0 + S <= 0) continue;
                int lxL = lx0, lzT = lz0;
                int lxR = lx0 + S, lzB = lz0 + S;
                if (lxR > bw) lxR = bw;      /* clamp sample, vertex stays at edge */
                if (lzB > bh) lzB = bh;
                if (lxL < 0) lxL = 0;
                if (lzT < 0) lzT = 0;
                int csxR = lxR < bw ? lxR : bw - 1;
                int cszB = lzB < bh ? lzB : bh - 1;

                /* Corner samples: heights, surface types, biomes. */
                int h00 = qb_region_value(heights, region_w, lxL, lzT, chunk_stride);
                int h10 = qb_region_value(heights, region_w, csxR, lzT, chunk_stride);
                int h01 = qb_region_value(heights, region_w, lxL, cszB, chunk_stride);
                int h11 = qb_region_value(heights, region_w, csxR, cszB, chunk_stride);
                int s00 = qb_region_value(types, region_w, lxL, lzT, chunk_stride);
                int s10 = qb_region_value(types, region_w, csxR, lzT, chunk_stride);
                int s01 = qb_region_value(types, region_w, lxL, cszB, chunk_stride);
                int s11 = qb_region_value(types, region_w, csxR, cszB, chunk_stride);
                int b00 = qb_region_value(biomes, region_w, lxL, lzT, chunk_stride);
                int b10 = qb_region_value(biomes, region_w, csxR, lzT, chunk_stride);
                int b01 = qb_region_value(biomes, region_w, lxL, cszB, chunk_stride);
                int b11 = qb_region_value(biomes, region_w, csxR, cszB, chunk_stride);

                /* Effective heights: clamp below-sea corners to sea level so
                 * shorelines sit flat at the water surface. */
                int e00 = h00 < sea ? sea : h00;
                int e10 = h10 < sea ? sea : h10;
                int e01 = h01 < sea ? sea : h01;
                int e11 = h11 < sea ? sea : h11;
                int all_water = (h00 <= sea && h10 <= sea && h01 <= sea && h11 <= sea);

                /* TOP QUAD (per-corner texture = corner blend) */
                int t00 = qb_tile_for(s00, b00);
                int t10 = qb_tile_for(s10, b10);
                int t01 = qb_tile_for(s01, b01);
                int t11 = qb_tile_for(s11, b11);
                float fx0 = (float)qb_max(wx, origin_x);
                float fz0 = (float)qb_max(wz, origin_z);
                float fx1 = (float)qb_min(wx + S, origin_x + bw);
                float fz1 = (float)qb_min(wz + S, origin_z + bh);
                int top_already_emitted = S == 1 && s1_seen != NULL && s1_seen[lzT * bw + lxL];
                if (!top_already_emitted && count >= capacity) { free(s1_seen); *out_count = count; return -2; }
                if (!top_already_emitted) {
                    QbQuad *q = &out[count];
                if (S == 1) {
                    /* Exact Cubiomes heightfield: one flat, integer-height
                     * block top per column. Greedily expand within this
                     * 16x16 section when height and material are identical. */
                    int tile = qb_tile_for(s00, b00);
                    int y = h00 < sea ? sea : h00;
                    if (h00 <= sea) tile = QB_TILE_WATER;
                    int width = 1, height = 1;
                    if (s1_seen != NULL) {
                        int section_x_end = wx + (16 - (wx - qb_floor_div_pow2(wx, 16) * 16));
                        int section_z_end = wz + (16 - (wz - qb_floor_div_pow2(wz, 16) * 16));
                        int world_x_end = qb_min(origin_x + bw, section_x_end);
                        int world_z_end = qb_min(origin_z + bh, section_z_end);
                        int probe_y, probe_tile;
                        while (wx + width < world_x_end &&
                               qb_s1_top_key(heights, types, biomes, region_w, bw, bh,
                                   origin_x, origin_z, center_x, center_z, prev_max, maxd,
                                   wx + width, wz, chunk_stride, &probe_y, &probe_tile) &&
                               probe_y == y && probe_tile == tile) width++;
                        for (;;) {
                            if (wz + height >= world_z_end) break;
                            int row_ok = 1;
                            for (int dx = 0; dx < width; dx++) {
                                if (!qb_s1_top_key(heights, types, biomes, region_w, bw, bh,
                                        origin_x, origin_z, center_x, center_z, prev_max, maxd,
                                        wx + dx, wz + height, chunk_stride, &probe_y, &probe_tile) ||
                                    probe_y != y || probe_tile != tile) { row_ok = 0; break; }
                            }
                            if (!row_ok) break;
                            height++;
                        }
                        for (int dz = 0; dz < height; dz++)
                            memset(s1_seen + (lzT + dz) * bw + lxL, 1, (size_t)width);
                    }
                    qb_set_vertex(&q->v[0], fx0, (float)y, fz0, tile, 0, QB_VFLAG_TOP);
                    qb_set_vertex(&q->v[1], fx0 + width, (float)y, fz0, tile, 0, QB_VFLAG_TOP);
                    qb_set_vertex(&q->v[2], fx0 + width, (float)y, fz0 + height, tile, 0, QB_VFLAG_TOP);
                    qb_set_vertex(&q->v[3], fx0, (float)y, fz0 + height, tile, 0, QB_VFLAG_TOP);
                } else if (all_water) {
                    qb_set_vertex(&q->v[0], fx0, (float)sea, fz0, QB_TILE_WATER, 0, QB_VFLAG_TOP);
                    qb_set_vertex(&q->v[1], fx1, (float)sea, fz0, QB_TILE_WATER, 0, QB_VFLAG_TOP);
                    qb_set_vertex(&q->v[2], fx1, (float)sea, fz1, QB_TILE_WATER, 0, QB_VFLAG_TOP);
                    qb_set_vertex(&q->v[3], fx0, (float)sea, fz1, QB_TILE_WATER, 0, QB_VFLAG_TOP);
                } else {
                    qb_set_vertex(&q->v[0], fx0, (float)e00, fz0, t00, 0, QB_VFLAG_TOP);
                    qb_set_vertex(&q->v[1], fx1, (float)e10, fz0, t10, 0, QB_VFLAG_TOP);
                    qb_set_vertex(&q->v[2], fx1, (float)e11, fz1, t11, 0, QB_VFLAG_TOP);
                    qb_set_vertex(&q->v[3], fx0, (float)e01, fz1, t01, 0, QB_VFLAG_TOP);
                }
                count++;
                out_tier_counts[t]++;
                }

                if (S == 1 && h00 > sea) {
                    int north = lzT > 0 ? qb_region_value(heights, region_w, lxL, lzT - 1, chunk_stride) : sea;
                    int south = lzT + 1 < bh ? qb_region_value(heights, region_w, lxL, lzT + 1, chunk_stride) : sea;
                    int west  = lxL > 0 ? qb_region_value(heights, region_w, lxL - 1, lzT, chunk_stride) : sea;
                    int east  = lxL + 1 < bw ? qb_region_value(heights, region_w, lxL + 1, lzT, chunk_stride) : sea;
                    int top = h00;
                    if (north < sea) north = sea; if (south < sea) south = sea;
                    if (west < sea) west = sea; if (east < sea) east = sea;
                    if (top > north && qb_emit_wall(out, capacity, &count, &out_tier_counts[t], fx1, fz0, fx0, fz0, top, north, s00)) { free(s1_seen); return -2; }
                    if (top > south && qb_emit_wall(out, capacity, &count, &out_tier_counts[t], fx0, fz1, fx1, fz1, top, south, s00)) { free(s1_seen); return -2; }
                    if (top > west  && qb_emit_wall(out, capacity, &count, &out_tier_counts[t], fx0, fz0, fx0, fz1, top, west, s00)) { free(s1_seen); return -2; }
                    if (top > east  && qb_emit_wall(out, capacity, &count, &out_tier_counts[t], fx1, fz1, fx1, fz0, top, east, s00)) { free(s1_seen); return -2; }
                }

                /* Coarser cells do not emit transition skirts; one-block cells emit exposed walls above. */
            }
        }
        free(s1_seen);
        prev_max = maxd;
    }
    *out_count = count;
    return 0;
}

int qb_bake_region(const int *heights, const int *types, const int *biomes,
                   int region_w, int region_h, int origin_x, int origin_z,
                   int center_x, int center_z, const QbTier *tiers, int tier_count,
                   QbQuad *out, int capacity, int *out_count, int *out_tier_counts) {
    return qb_bake_region_layout(heights, types, biomes, region_w, region_h,
            origin_x, origin_z, center_x, center_z, tiers, tier_count,
            out, capacity, out_count, out_tier_counts, 256);
}

int qb_bake_region_interleaved(const int *region,
                               int region_w, int region_h,
                               int origin_x, int origin_z,
                               int center_x, int center_z,
                               const QbTier *tiers, int tier_count,
                               QbQuad *out, int capacity,
                               int *out_count, int *out_tier_counts) {
    if (!region) return -1;
    return qb_bake_region_layout(region, region + 256, region + 512,
            region_w, region_h, origin_x, origin_z, center_x, center_z,
            tiers, tier_count, out, capacity, out_count, out_tier_counts, 768);
}
