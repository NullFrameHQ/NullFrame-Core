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
static void place_giant_mushroom(VegBuffer *vb, int x, int z, int ground, int height, int red) {
    int base = ground + 1;
    if (!red) {
        const int radius = 3;
        for (int dz = -radius; dz <= radius; dz++) for (int dx = -radius; dx <= radius; dx++) {
            int edge_x = abs(dx) == radius, edge_z = abs(dz) == radius;
            if (edge_x && edge_z) continue;
            int west = dx == -radius || (edge_z && dx == 1 - radius);
            int east = dx == radius || (edge_z && dx == radius - 1);
            int north = dz == -radius || (edge_x && dz == 1 - radius);
            int south = dz == radius || (edge_x && dz == radius - 1);
            vegbuf_emit(vb, x + dx, base + height, z + dz,
                    BLK_BROWN_CAP_BASE + west + 2 * east + 4 * north + 8 * south + 16);
        }
    } else {
        for (int dy = height - 3; dy <= height; dy++) {
            int radius = dy < height ? 2 : 1;
            for (int dz = -radius; dz <= radius; dz++) for (int dx = -radius; dx <= radius; dx++) {
                int edge_x = abs(dx) == radius, edge_z = abs(dz) == radius;
                if (dy < height && edge_x == edge_z) continue;
                int faces = (dx < 0) + 2 * (dx > 0) + 4 * (dz < 0) + 8 * (dz > 0)
                        + 16 * (dy >= height - 1);
                vegbuf_emit(vb, x + dx, base + dy, z + dz, BLK_RED_CAP_BASE + faces);
            }
        }
    }
    for (int dy = 0; dy < height; dy++) vegbuf_emit(vb, x, base + dy, z, BLK_MUSHROOM_STEM);
}

static void generate_mushrooms(uint64_t seed, int cx, int cz, const int *heights, const int *biomes,
        const int *types, int bottom, int top, VegBuffer *vb) {
    int present = 0;
    for (int i = 0; i < 256; i++) if (biomes[i] == mushroom_fields || biomes[i] == mushroom_field_shore) { present = 1; break; }
    if (!present) return;
    /* One giant attempt/chunk, 50:50 red/brown. Keep full caps inside the chunk-owned workspace. */
    uint64_t hash = vegetation_hash(seed, cx, cz, mushroom_fields, 0x630ADC4EBF195872ULL);
    int x = 3 + floor_mod_u64(hash, 10), z = 3 + floor_mod_u64(rotate_left_64(hash, 21), 10);
    int at = z * 16 + x, ground = heights[at];
    int height = 4 + floor_mod_u64(rotate_left_64(hash, 37), 3);
    if (floor_mod_u64(rotate_left_64(hash, 11), 12) == 0) height *= 2;
    int red = floor_mod_u64(rotate_left_64(hash, 29), 2), clear = 1;
    if ((biomes[at] == mushroom_fields || biomes[at] == mushroom_field_shore)
            && types[at] != SURFACE_WATER && ground >= bottom && ground + height + 2 < top) {
        int radius = red ? 2 : 3;
        for (int dz = -radius; dz <= radius; dz++) for (int dx = -radius; dx <= radius; dx++) {
            int i = (z + dz) * 16 + x + dx;
            int lowest_cap = ground + 1 + (red ? height - 3 : height);
            if (heights[i] >= lowest_cap) clear = 0;
        }
        if (clear) place_giant_mushroom(vb, x, z, ground, height, red);
    }
    /* The biome's small-mushroom random patches: bounded scatter on its mycelium surface. */
    for (int color = 0; color < 2; color++) {
        uint64_t patch = vegetation_hash(seed, cx, cz, mushroom_fields, 0x8A49D30F2B6C175EULL + color);
        if (floor_mod_u64(patch, color ? 256 : 4) != 0) continue;
        for (int n = 0; n < 16; n++) {
            uint64_t p = vegetation_hash(seed, cx, cz, mushroom_fields, patch + (uint64_t)n * 0x9E3779B97F4A7C15ULL);
            int i = floor_mod_u64(p, 256), y = heights[i] + 1;
            if ((biomes[i] != mushroom_fields && biomes[i] != mushroom_field_shore)
                    || types[i] == SURFACE_WATER || y < bottom || y >= top) continue;
            /* Do not stamp a small mushroom into the giant stem. */
            if (i == at && clear) continue;
            vegbuf_emit(vb, i & 15, y, i >> 4, color ? BLK_RED_MUSHROOM : BLK_BROWN_MUSHROOM);
        }
    }
}
