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

#ifndef NULLFRAME_VEGETATION_INTERNAL_H
#define NULLFRAME_VEGETATION_INTERNAL_H

#include "core_types.h"
#define VEG_DENSITY_DENOMINATOR 10000

typedef struct {
    int *data;       /* points into the output array */
    int capacity;    /* max blocks */
    int count;       /* current block count */
    int use_templates; /* combined chunks use shared native shape templates */
} VegBuffer;

static inline uint64_t vegetation_hash(uint64_t seed, int world_x, int world_z, int biome_id, uint64_t salt) {
    uint64_t hash = seed;
    hash ^= ((uint64_t)(int64_t)world_x * (uint64_t)341873128712ULL);
    hash ^= ((uint64_t)(int64_t)world_z * (uint64_t)132897987541ULL);
    hash ^= ((uint64_t)(int32_t)biome_id * 0x9E3779B97F4A7C15ULL);
    hash ^= salt;
    hash ^= (hash >> 30);
    hash *= 0xBF58476D1CE4E5B9ULL;
    hash ^= (hash >> 27);
    hash *= 0x94D049BB133111EBULL;
    hash ^= (hash >> 31);
    return hash;
}

static inline uint64_t rotate_left_64(uint64_t x, int n) {
    return (x << n) | (x >> (64 - n));
}

static inline int floor_mod_u64(uint64_t a, int b) {
    /* Java's Math.floorMod(long, int) works with SIGNED longs.
     * C's uint64_t % int works with UNSIGNED values — different math.
     * Treat the hash as signed, then adjust to floor division. */
    int64_t signed_a = (int64_t)a;
    int mod = (int)(signed_a % (int64_t)b);
    if (mod < 0) mod += b;
    return mod;
}

static inline int bounded_height(uint64_t hash, int min_inclusive, int max_inclusive) {
    /* Java's Math.floorMod(hash, range) — must use floor mod, not C's % */
    int range = max_inclusive - min_inclusive + 1;
    return min_inclusive + floor_mod_u64(hash, range);
}

static inline void choose_horizontal_offset(uint64_t hash, int *dx, int *dz) {
    switch (floor_mod_u64(hash, 4)) {
        case 0: *dx = 1;  *dz = 0;  break;
        case 1: *dx = -1; *dz = 0;  break;
        case 2: *dx = 0;  *dz = 1;  break;
        default: *dx = 0; *dz = -1; break;
    }
}

static inline void vegbuf_init(VegBuffer *vb, int *data, int capacity) {
    vb->data = data;
    vb->capacity = capacity;
    vb->count = 0;
    vb->use_templates = 0;
    /* data[0] will be set to count at flush time */
}

static inline void vegbuf_emit(VegBuffer *vb, int local_x, int y, int local_z, int block_type) {
    if (vb->count >= vb->capacity) return;  /* overflow guard */
    if (block_type == BLK_AIR) return;
    if (local_x < 0 || local_x >= 16 || local_z < 0 || local_z >= 16) return;
    int idx = 1 + vb->count * 4;
    vb->data[idx]     = local_x;
    vb->data[idx + 1] = y;
    vb->data[idx + 2] = local_z;
    vb->data[idx + 3] = block_type;
    vb->count++;
}

static inline void vegbuf_flush(VegBuffer *vb) {
    vb->data[0] = vb->count;
}

#endif
