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
#include "tree_shapes.h"

/* Tree placement functions (ported from Java, write to VegBuffer) */

static void place_vertical_log_column(VegBuffer *vb, int local_x, int local_z,
                                       int start_y, int height, int log_block,
                                       int bottom_y, int top_y_exclusive) {
    for (int i = 0; i < height; i++) {
        int y = start_y + i;
        if (y >= bottom_y && y < top_y_exclusive) {
            vegbuf_emit(vb, local_x, y, local_z, log_block);
        }
    }
}

static void place_leaf_square(VegBuffer *vb, int center_x, int y, int center_z,
                              int radius, int leaf_block, int skip_far_corners,
                              int bottom_y, int top_y_exclusive) {
    if (y < bottom_y || y >= top_y_exclusive) return;
    for (int dx = -radius; dx <= radius; dx++) {
        for (int dz = -radius; dz <= radius; dz++) {
            if (skip_far_corners && abs(dx) == radius && abs(dz) == radius) continue;
            vegbuf_emit(vb, center_x + dx, y, center_z + dz, leaf_block);
        }
    }
}

static void place_leaf_cross(VegBuffer *vb, int center_x, int y, int center_z,
                             int radius, int leaf_block,
                             int bottom_y, int top_y_exclusive) {
    if (y >= bottom_y && y < top_y_exclusive) {
        vegbuf_emit(vb, center_x, y, center_z, leaf_block);
    }
    for (int i = 1; i <= radius; i++) {
        if (y >= bottom_y && y < top_y_exclusive) {
            vegbuf_emit(vb, center_x + i, y, center_z, leaf_block);
            vegbuf_emit(vb, center_x - i, y, center_z, leaf_block);
            vegbuf_emit(vb, center_x, y, center_z + i, leaf_block);
            vegbuf_emit(vb, center_x, y, center_z - i, leaf_block);
        }
    }
}

static void maybe_hang_leaf(VegBuffer *vb, int local_x, int y, int local_z,
                            int leaf_block, uint64_t hash,
                            int bottom_y, int top_y_exclusive) {
    if (floor_mod_u64(hash, 100) < 50) {
        if (y >= bottom_y && y < top_y_exclusive) {
            vegbuf_emit(vb, local_x, y, local_z, leaf_block);
        }
    }
}

/*
 * Four-layer blob canopy. Radius increases below the top, and hashed corner
 * removal varies the outline while keeping generation deterministic.
 */
static void place_blob_tree(VegBuffer *vb, int local_x, int local_z, int terrain_y,
                            int trunk_height, int canopy_radius,
                            int log_block, int leaf_block, uint64_t hash,
                            int bottom_y, int top_y_exclusive) {
    place_vertical_log_column(vb, local_x, local_z, terrain_y + 1, trunk_height,
                              log_block, bottom_y, top_y_exclusive);

    int top_y = terrain_y + trunk_height;
    for (int layer = 0; layer < 4; layer++) {
        int i = -layer;
        int radius = canopy_radius - 1 - (i / 2);
        if (radius < 0) radius = 0;
        int layer_y = top_y + i;
        for (int dz = -radius; dz <= radius; dz++) {
            for (int dx = -radius; dx <= radius; dx++) {
                if (radius > 0 && abs(dx) == radius && abs(dz) == radius) {
                    /* vanilla isInvalidForLeaves: corners removed when
                       random<50% (always on the top layer, i == 0) */
                    if (i == 0) continue;
                    uint64_t ch = rotate_left_64(hash, 7 + layer * 13)
                                ^ ((uint64_t)(dx * 73856093) ^ (uint64_t)(dz * 19349663));
                    if (floor_mod_u64(ch, 2) == 0) continue;
                }
                vegbuf_emit(vb, local_x + dx, layer_y, local_z + dz, leaf_block);
            }
        }
    }
}

/* Oak with a six- to eight-block trunk and a raised blob canopy. */
void place_oak_tree(VegBuffer *vb, int local_x, int local_z, int terrain_y,
                           int log_block, int leaf_block, uint64_t hash,
                           int bottom_y, int top_y_exclusive) {
    int trunk_height = 6 + floor_mod_u64(hash, 3);
    place_blob_tree(vb, local_x, local_z, terrain_y, trunk_height, 2,
                    log_block, leaf_block, hash, bottom_y, top_y_exclusive);
}

void place_windswept_oak_tree(VegBuffer *vb, int x, int z, int y,
        int log, int leaf, uint64_t hash, int bottom, int top) {
    int height = 4 + floor_mod_u64(hash, 3);
    place_blob_tree(vb, x, z, y, height, 2, log, leaf, hash, bottom, top);
    /* Vanilla foliage cannot replace logs, including the low canopy of a four-block oak. */
    place_vertical_log_column(vb, x, z, y + 1, height, log, bottom, top);
}

/* Birch with a six- to eight-block trunk and a blob canopy. */
void place_birch_tree(VegBuffer *vb, int local_x, int local_z, int terrain_y,
                             int log_block, int leaf_block, uint64_t hash,
                             int bottom_y, int top_y_exclusive) {
    int trunk_height = 6 + floor_mod_u64(rotate_left_64(hash, 17), 3);
    place_blob_tree(vb, local_x, local_z, terrain_y, trunk_height, 2,
                    log_block, leaf_block, hash, bottom_y, top_y_exclusive);
}

/* Swamp oak with a taller trunk, wider canopy and hanging leaves. */
void place_swamp_oak_tree(VegBuffer *vb, int local_x, int local_z, int terrain_y,
                                 int log_block, int leaf_block, uint64_t hash,
                                 int bottom_y, int top_y_exclusive) {
    int trunk_height = 7 + floor_mod_u64(rotate_left_64(hash, 7), 4);
    place_blob_tree(vb, local_x, local_z, terrain_y, trunk_height, 3,
                    log_block, leaf_block, hash, bottom_y, top_y_exclusive);
    /* dangling leaves just below the canopy blob (swampy look) */
    int canopy_base_y = terrain_y + trunk_height - 3;
    maybe_hang_leaf(vb, local_x + 2, canopy_base_y - 1, local_z, leaf_block, hash, bottom_y, top_y_exclusive);
    maybe_hang_leaf(vb, local_x - 2, canopy_base_y - 1, local_z, leaf_block, rotate_left_64(hash, 13), bottom_y, top_y_exclusive);
    maybe_hang_leaf(vb, local_x, canopy_base_y - 1, local_z + 2, leaf_block, rotate_left_64(hash, 27), bottom_y, top_y_exclusive);
    maybe_hang_leaf(vb, local_x, canopy_base_y - 1, local_z - 2, leaf_block, rotate_left_64(hash, 41), bottom_y, top_y_exclusive);
}

/* Spruce canopy built in layers with a changing radius and trimmed corners. */
static void place_spruce_tree_impl(VegBuffer *vb, int local_x, int local_z, int terrain_y,
                              int log_block, int leaf_block, uint64_t hash,
                              int bottom_y, int top_y_exclusive, int vanilla_size) {
    /* trunk 8-12 (vanilla 5-8, raised for walk clearance) */
    int trunk_height = (vanilla_size ? 5 : 8) + floor_mod_u64(hash, 3) + floor_mod_u64(rotate_left_64(hash, 9), 2);
    place_vertical_log_column(vb, local_x, local_z, terrain_y + 1, trunk_height, log_block, bottom_y, top_y_exclusive);

    /* SpruceFoliagePlacer(Uniform(2,3), Uniform(0,2), Uniform(1,2)) */
    int radius = 2 + floor_mod_u64(rotate_left_64(hash, 5), 2);      /* 2-3 */
    int offset = floor_mod_u64(rotate_left_64(hash, 11), 3);          /* 0-2 */
    /* Keep the foliage four to eight blocks tall and clear of the lower trunk. */
    int foliage_height = trunk_height - (vanilla_size ? 1 + floor_mod_u64(rotate_left_64(hash, 17), 2) : 3);
    if (foliage_height < 4) foliage_height = 4;
    if (foliage_height > 8) foliage_height = 8;

    int top_y = terrain_y + trunk_height;
    int i = floor_mod_u64(rotate_left_64(hash, 23), 2);  /* 0-1 */
    int j = 1, k = 0;
    for (int l = offset; l >= -foliage_height; l--) {
        int layer_y = top_y + l;
        /* Vanilla generateSquare with isInvalidForLeaves = corners removed */
        if (i == 0) {
            vegbuf_emit(vb, local_x, layer_y, local_z, leaf_block);
        } else {
            for (int dz = -i; dz <= i; dz++) {
                for (int dx = -i; dx <= i; dx++) {
                    if (abs(dx) == i && abs(dz) == i) continue;  /* corners */
                    vegbuf_emit(vb, local_x + dx, layer_y, local_z + dz, leaf_block);
                }
            }
        }
        /* vanilla state machine */
        if (i >= j) { i = k; k = 1; j = (j + 1 < radius) ? j + 1 : radius; }
        else { i++; }
    }
    vegbuf_emit(vb, local_x, top_y + 1, local_z, leaf_block);
    if (vanilla_size) place_vertical_log_column(vb, local_x, local_z, terrain_y + 1,
            trunk_height, log_block, bottom_y, top_y_exclusive);
}

void place_spruce_tree(VegBuffer *vb, int x, int z, int y,
        int log, int leaf, uint64_t hash, int bottom, int top) {
    place_spruce_tree_impl(vb, x, z, y, log, leaf, hash, bottom, top, 0);
}
void place_windswept_spruce_tree(VegBuffer *vb, int x, int z, int y,
        int log, int leaf, uint64_t hash, int bottom, int top) {
    place_spruce_tree_impl(vb, x, z, y, log, leaf, hash, bottom, top, 1);
}

/* Fancy oak with branch logs and a five-layer rounded canopy. */
void place_fancy_oak_tree(VegBuffer *vb, int local_x, int local_z, int terrain_y,
                                 int log_block, int leaf_block, uint64_t hash,
                                 int bottom_y, int top_y_exclusive) {
    int trunk_height = 7 + floor_mod_u64(hash, 8);   /* 7-14 (vanilla 3-13) */
    place_vertical_log_column(vb, local_x, local_z, terrain_y + 1, trunk_height,
                              log_block, bottom_y, top_y_exclusive);
    /* 2-4 branch logs at random heights/offsets (LargeOakTrunkPlacer feel) */
    int branches = 2 + floor_mod_u64(rotate_left_64(hash, 7), 3);  /* 2-4 */
    for (int b = 0; b < branches; b++) {
        int by = terrain_y + 2 + floor_mod_u64(rotate_left_64(hash, 13 + b * 9), trunk_height - 2);
        int bdx, bdz;
        choose_horizontal_offset(rotate_left_64(hash, 21 + b * 11), &bdx, &bdz);
        vegbuf_emit(vb, local_x + bdx, by, local_z + bdz, log_block);
    }
    int top_y = terrain_y + trunk_height;
    /* 5 layers, radius 2 with middle 3, circular cut */
    for (int layer = 0; layer < 5; layer++) {
        int layer_y = top_y - layer;
        int r = (layer == 0 || layer == 4) ? 2 : 3;
        for (int dz = -r; dz <= r; dz++) {
            for (int dx = -r; dx <= r; dx++) {
                if (dx * dx + dz * dz > r * r) continue;
                vegbuf_emit(vb, local_x + dx, layer_y, local_z + dz, leaf_block);
            }
        }
    }
}

/* Tall birch variant with a six- to fourteen-block trunk and blob canopy. */
void place_super_birch_tree(VegBuffer *vb, int local_x, int local_z, int terrain_y,
                                   int log_block, int leaf_block, uint64_t hash,
                                   int bottom_y, int top_y_exclusive) {
    int trunk_height = 6 + floor_mod_u64(hash, 3) + floor_mod_u64(rotate_left_64(hash, 9), 6); /* 6-14 (vanilla 5-13) */
    place_blob_tree(vb, local_x, local_z, terrain_y, trunk_height, 2,
                    log_block, leaf_block, hash, bottom_y, top_y_exclusive);
}

/* Pine with a seven- to eleven-block trunk and a narrow crown. */
void place_pine_tree(VegBuffer *vb, int local_x, int local_z, int terrain_y,
                            int log_block, int leaf_block, uint64_t hash,
                            int bottom_y, int top_y_exclusive) {
    int trunk_height = 7 + floor_mod_u64(hash, 5);   /* 7-11 (vanilla 6-10) */
    place_vertical_log_column(vb, local_x, local_z, terrain_y + 1, trunk_height, log_block, bottom_y, top_y_exclusive);

    int radius = 1;                       /* ConstantIntProvider(1) */
    int offset = 1;                       /* ConstantIntProvider(1) */
    int foliage_height = 3 + floor_mod_u64(rotate_left_64(hash, 9), 2); /* 3-4 */
    int top_y = terrain_y + trunk_height;
    int i = 0;
    for (int l = offset; l >= offset - foliage_height; l--) {
        int layer_y = top_y + l;
        if (i == 0) {
            vegbuf_emit(vb, local_x, layer_y, local_z, leaf_block);
        } else {
            for (int dz = -i; dz <= i; dz++) {
                for (int dx = -i; dx <= i; dx++) {
                    if (abs(dx) == i && abs(dz) == i) continue;
                    vegbuf_emit(vb, local_x + dx, layer_y, local_z + dz, leaf_block);
                }
            }
        }
        if (i >= 1 && l == offset - foliage_height + 1) { i--; }
        else if (i < radius) { i++; }
    }
}

/* Mega pine with a 2 by 2 trunk and a crown that widens down from the tip. */
void place_mega_pine_tree(VegBuffer *vb, int local_x, int local_z, int terrain_y,
                                 int log_block, int leaf_block, uint64_t hash,
                                 int bottom_y, int top_y_exclusive) {
    /* Choose a trunk height from 13 through 20 blocks. */
    int trunk_height = 13 + floor_mod_u64(hash, 8);   /* 13-20 */
    int crown_height = 13 + floor_mod_u64(rotate_left_64(hash, 7), 5);  /* 13-17 */
    /* Clamp crown so its bottom stays >=3 above ground (walk clearance):
     * crown bottom = trunk_height - crown_height, so crown <= trunk - 3. */
    if (crown_height > trunk_height - 3) crown_height = trunk_height - 3;
    /* 2x2 trunk */
    for (int dz = 0; dz < 2; dz++) {
        for (int dx = 0; dx < 2; dx++) {
            place_vertical_log_column(vb, local_x + dx, local_z + dz,
                                      terrain_y + 1, trunk_height, log_block,
                                      bottom_y, top_y_exclusive);
        }
    }
    int top_y = terrain_y + trunk_height;
    int cx = local_x + 1;  /* 2x2 center offset */
    int cz = local_z + 1;
    for (int j = 0; j <= crown_height; j++) {
        int layer_y = top_y + j - crown_height;   /* from tip down to top */
        int k = top_y - layer_y;                  /* distance from top */
        int r = (int)((float)k / (float)crown_height * 3.5f);
        if (k > 0 && r == 0 && (j & 1) == 0) r = 1;  /* vanilla +1 jitter */
        for (int dz = -r; dz <= r; dz++) {
            for (int dx = -r; dx <= r; dx++) {
                int ax = abs(dx), az = abs(dz);
                if (ax + az >= 7) continue;
                if (ax * ax + az * az > r * r) continue;
                vegbuf_emit(vb, cx + dx, layer_y, cz + dz, leaf_block);
            }
        }
    }
}

void place_dark_oak_tree(VegBuffer *vb, int local_x, int local_z, int terrain_y,
                                int log_block, int leaf_block, uint64_t hash,
                                int bottom_y, int top_y_exclusive) {
    /* DarkOakTrunkPlacer(6, 2, 1) → 6-10 */
    int trunk_height = 6 + floor_mod_u64(hash, 3) + floor_mod_u64(rotate_left_64(hash, 9), 2);
    int can_use_2x2 = (local_x >= 1 && local_x <= 14 && local_z >= 1 && local_z <= 14);
    if (can_use_2x2) {
        place_vertical_log_column(vb, local_x, local_z, terrain_y + 1, trunk_height, log_block, bottom_y, top_y_exclusive);
        place_vertical_log_column(vb, local_x + 1, local_z, terrain_y + 1, trunk_height, log_block, bottom_y, top_y_exclusive);
        place_vertical_log_column(vb, local_x, local_z + 1, terrain_y + 1, trunk_height, log_block, bottom_y, top_y_exclusive);
        place_vertical_log_column(vb, local_x + 1, local_z + 1, terrain_y + 1, trunk_height, log_block, bottom_y, top_y_exclusive);
    } else {
        place_vertical_log_column(vb, local_x, local_z, terrain_y + 1, trunk_height, log_block, bottom_y, top_y_exclusive);
    }

    int canopy_base_y = terrain_y + trunk_height - 1;
    int canopy_radius = can_use_2x2 ? 3 : 2;
    place_leaf_square(vb, local_x, canopy_base_y, local_z, canopy_radius, leaf_block, 1, bottom_y, top_y_exclusive);
    place_leaf_square(vb, local_x, canopy_base_y + 1, local_z, 2, leaf_block, 0, bottom_y, top_y_exclusive);
    place_leaf_cross(vb, local_x, canopy_base_y + 2, local_z, 1, leaf_block, bottom_y, top_y_exclusive);
    if (can_use_2x2) {
        vegbuf_emit(vb, local_x + 1, canopy_base_y + 2, local_z, leaf_block);
        vegbuf_emit(vb, local_x, canopy_base_y + 2, local_z + 1, leaf_block);
        vegbuf_emit(vb, local_x + 1, canopy_base_y + 2, local_z + 1, leaf_block);
    }
}

void place_acacia_tree(VegBuffer *vb, int local_x, int local_z, int terrain_y,
                              int log_block, int leaf_block, uint64_t hash,
                              int bottom_y, int top_y_exclusive) {
    /* Acacia trunk with a horizontal fork; height is five through nine blocks. */
    int trunk_height = 5 + floor_mod_u64(hash, 3) + floor_mod_u64(rotate_left_64(hash, 11), 3);
    int dx, dz;
    choose_horizontal_offset(hash, &dx, &dz);
    int bend_y = terrain_y + (trunk_height - 2 > 3 ? trunk_height - 2 : 3);
    place_vertical_log_column(vb, local_x, local_z, terrain_y + 1, bend_y - terrain_y, log_block, bottom_y, top_y_exclusive);
    vegbuf_emit(vb, local_x + dx, bend_y + 1, local_z + dz, log_block);
    vegbuf_emit(vb, local_x + dx, bend_y + 2, local_z + dz, log_block);

    int canopy_x = local_x + dx;
    int canopy_z = local_z + dz;
    int canopy_y = bend_y + 2;
    place_leaf_square(vb, canopy_x, canopy_y, canopy_z, 2, leaf_block, 1, bottom_y, top_y_exclusive);
    place_leaf_cross(vb, canopy_x, canopy_y + 1, canopy_z, 1, leaf_block, bottom_y, top_y_exclusive);
}

void place_jungle_tree(VegBuffer *vb, int local_x, int local_z, int terrain_y,
                              int log_block, int leaf_block, uint64_t hash,
                              int bottom_y, int top_y_exclusive) {
    /* Jungle trunk height is six through thirteen blocks. */
    int trunk_height = 6 + floor_mod_u64(hash, 8);
    int tall = (trunk_height >= 9) ? 1 : 0;
    place_vertical_log_column(vb, local_x, local_z, terrain_y + 1, trunk_height, log_block, bottom_y, top_y_exclusive);

    int canopy_base_y = terrain_y + trunk_height - (tall ? 2 : 1);
    int canopy_radius = tall ? 2 : 1;
    place_leaf_square(vb, local_x, canopy_base_y, local_z, canopy_radius, leaf_block, 1, bottom_y, top_y_exclusive);
    place_leaf_square(vb, local_x, canopy_base_y + 1, local_z, 1, leaf_block, 0, bottom_y, top_y_exclusive);
    place_leaf_cross(vb, local_x, canopy_base_y + 2, local_z, 1, leaf_block, bottom_y, top_y_exclusive);
    if (tall) {
        int dx1, dz1, dx2, dz2;
        choose_horizontal_offset(rotate_left_64(hash, 9), &dx1, &dz1);
        choose_horizontal_offset(rotate_left_64(hash, 23), &dx2, &dz2);
        vegbuf_emit(vb, local_x + dx1, canopy_base_y, local_z + dz1, leaf_block);
        vegbuf_emit(vb, local_x + dx2, canopy_base_y + 1, local_z + dz2, leaf_block);
    }
}

/*
 * Cherry canopy uses the shared foliage template, with deterministic holes and
 * hanging leaves. The trunk is seven or eight blocks tall. Side branches are not generated.
 */
#include "cherry_foliage_template.h"

void place_cherry_tree(VegBuffer *vb, int local_x, int local_z, int terrain_y,
                              int log_block, int leaf_block, uint64_t hash,
                              int bottom_y, int top_y_exclusive) {
    int trunk_height = 7 + (int) floor_mod_u64(hash, 2);            /* 7 or 8 */
    int trunk_top    = terrain_y + trunk_height;
    place_vertical_log_column(vb, local_x, local_z, terrain_y + 1, trunk_height,
                              log_block, bottom_y, top_y_exclusive);

    if (vb->use_templates) {
        for (size_t i = 0; i < sizeof(cherry_foliage_template) / sizeof(cherry_foliage_template[0]); i++) {
            CherryFoliageCell t = cherry_foliage_template[i];
            int y = trunk_top + t.y;
            if (y < bottom_y || y >= top_y_exclusive) continue;
            uint64_t cell = rotate_left_64(hash
                    ^ ((uint64_t)(t.x + 8) * 0x9E3779B97F4A7C15ULL)
                    ^ ((uint64_t)(t.z + 8) * 0xC2B2AE3D27D4EB4FULL)
                    ^ ((uint64_t)t.layer * 0x165667B19E3779F9ULL), 13);
            if ((t.flags & 1) && floor_mod_u64(cell, 100) < 25) continue;
            vegbuf_emit(vb, local_x + t.x, y, local_z + t.z, leaf_block);
            if ((t.flags & 2) && floor_mod_u64(rotate_left_64(cell, 29), 6) == 0 && y - 1 >= bottom_y)
                vegbuf_emit(vb, local_x + t.x, y - 1, local_z + t.z, leaf_block);
        }
        return;
    }

    /* 5 canopy layers (vanilla height=5), bottom -> top.
     * Radius narrow at the underside, 4-wide belly, tapering cap. */
    static const int cdy[5]  = {-3, -2, -1, 0, 1};
    static const int crad[5] = { 3,  4,  4, 3, 2};

    for (int L = 0; L < 5; L++) {
        int y = trunk_top + cdy[L];
        int r = crad[L];
        if (y < bottom_y || y >= top_y_exclusive) continue;
        for (int dz = -r; dz <= r; dz++) {
            for (int dx = -r; dx <= r; dx++) {
                int adx = dx < 0 ? -dx : dx;
                int adz = dz < 0 ? -dz : dz;

                /* Trim the outer corners of this canopy layer. */
                if (adx + adz > r + 1) continue;

                /* Stable per-cell hash so the two random rolls below are
                 * deterministic and vary across the canopy. */
                uint64_t cell = rotate_left_64(
                        hash
                      ^ ((uint64_t) (dx + 8) * 0x9E3779B97F4A7C15ULL)
                      ^ ((uint64_t) (dz + 8) * 0xC2B2AE3D27D4EB4FULL)
                      ^ ((uint64_t) L * 0x165667B19E3779F9ULL), 13);

                /* wide_bottom_layer_hole_chance 0.25 — belly only, rim stays. */
                if (L == 0 && adx < r && adz < r && floor_mod_u64(cell, 100) < 25) continue;

                vegbuf_emit(vb, local_x + dx, y, local_z + dz, leaf_block);

                /* hanging_leaves_chance 1/6 — a leaf dangling under the rim. */
                if (L == 0 && (adx == r || adz == r)
                    && floor_mod_u64(rotate_left_64(cell, 29), 6) == 0
                    && y - 1 >= bottom_y) {
                    vegbuf_emit(vb, local_x + dx, y - 1, local_z + dz, leaf_block);
                }
            }
        }
    }
}
