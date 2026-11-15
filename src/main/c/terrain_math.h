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

#ifndef NULLFRAME_TERRAIN_MATH_H
#define NULLFRAME_TERRAIN_MATH_H

#include "core_types.h"
static inline int normalize_dimension_id(int dimension_id) {
    if (dimension_id == CB_DIM_OVERWORLD ||
        dimension_id == CB_DIM_NETHER   ||
        dimension_id == CB_DIM_END) {
        return dimension_id;
    }
    return CB_DIM_CUSTOM;
}

static inline int floor_div_4(int value) {
    return value >= 0 ? value / 4 : -(((-value) + 3) / 4);
}

static inline int floor_div_world(int a, int b) {
    int q = a / b;
    if (a % b != 0 && a < 0) q--;
    return q;
}

static inline float value_noise_hash(int wx, int wz) {
    unsigned h = (unsigned)(wx * 0x1f123bb5u) ^ (unsigned)(wz * 0x27d4eb2du) ^ 0x165667b1u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return (h & 0xFFFFFFu) * (1.0f / 16777216.0f);
}

static inline int round_to_int(float value) {
    return value >= 0.0f ? (int) (value + 0.5f) : (int) (value - 0.5f);
}

static inline float bilerp(float h00, float h10, float h01, float h11, float tx, float tz) {
    float hx0 = h00 + (h10 - h00) * tx;
    float hx1 = h01 + (h11 - h01) * tx;
    return hx0 + (hx1 - hx0) * tz;
}

static inline float quintic_fade(float t) {
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

static inline int interpolate_biome_id_discrete(int b00, int b10, int b01, int b11, float tx, float tz) {
    float w00 = (1.0f - tx) * (1.0f - tz);
    float w10 = tx * (1.0f - tz);
    float w01 = (1.0f - tx) * tz;
    float w11 = tx * tz;

    int biome_ids[4] = {b00, b10, b01, b11};
    float biome_weights[4] = {w00, w10, w01, w11};

    int best_biome = b00;
    float best_weight = -1.0f;

    for (int i = 0; i < 4; i++) {
        int biome_id = biome_ids[i];
        float combined_weight = 0.0f;
        for (int j = 0; j < 4; j++) {
            if (biome_ids[j] == biome_id) {
                combined_weight += biome_weights[j];
            }
        }
        if (combined_weight > best_weight) {
            best_weight = combined_weight;
            best_biome = biome_id;
        }
    }
    return best_biome;
}

#endif
