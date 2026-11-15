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

#ifndef NULLFRAME_SURFACE_CLASSIFICATION_H
#define NULLFRAME_SURFACE_CLASSIFICATION_H

#include "core_types.h"
static inline int classify_surface_type(int biome_id, int surface_y) {
    (void) surface_y;

    /* Water: oceans, rivers */
    if (isOceanic(biome_id) || biome_id == river || biome_id == frozen_river) {
        return SURFACE_WATER;
    }

    /* Snow: use Cubiomes' isSnowy helper + explicit snowy mountain biomes */
    if (isSnowy(biome_id) ||
        biome_id == snowy_beach ||
        biome_id == snowy_mountains ||
        biome_id == ice_spikes ||
        biome_id == grove ||
        biome_id == snowy_slopes ||
        biome_id == frozen_peaks) {
        return SURFACE_SNOW;
    }

    /* Sand: deserts, beaches, badlands (isMesa) */
    if (isMesa(biome_id) ||
        biome_id == beach ||
        biome_id == desert ||
        biome_id == desert_hills ||
        biome_id == desert_lakes ||
        biome_id == mushroom_field_shore) {
        return SURFACE_SAND;
    }

    /* Stone: rocky peaks, stone shores */
    if (biome_id == stone_shore || biome_id == gravelly_mountains ||
        biome_id == stony_peaks ||
        biome_id == jagged_peaks) {
        return SURFACE_STONE;
    }

    return SURFACE_GRASS;
}

/*
 * Small cache of surface classifications by biome ID. Tags distinguish collisions;
 * surface_y is ignored by classify_surface_type.
 */
typedef struct {
    int tags[16];
    int types[16];
} ClassifyMemo;

static inline void classify_memo_init(ClassifyMemo *m) {
    for (int i = 0; i < 16; i++) m->tags[i] = -1;
}

static inline int classify_memo_get(ClassifyMemo *m, int biome_id) {
    int slot = biome_id & 15;
    if (m->tags[slot] == biome_id) return m->types[slot];
    int t = classify_surface_type(biome_id, 0);
    m->tags[slot] = biome_id;
    m->types[slot] = t;
    return t;
}

#endif
