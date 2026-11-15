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
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

typedef void (*TreeShape)(VegBuffer *, int, int, int, int, int, uint64_t, int, int);
typedef struct { int8_t x, y, z; uint8_t material; } TreeCell;
typedef struct { uint64_t key; int count; TreeCell cells[]; } TreeTemplate;
static TreeTemplate *volatile tree_templates[1024];

static TreeTemplate *tree_template_load(int slot) {
#ifdef _WIN32
    return (TreeTemplate *)InterlockedCompareExchangePointer((PVOID volatile *)&tree_templates[slot], NULL, NULL);
#else
    return __atomic_load_n(&tree_templates[slot], __ATOMIC_ACQUIRE);
#endif
}
static int tree_template_install(int slot, TreeTemplate *value) {
#ifdef _WIN32
    return InterlockedCompareExchangePointer((PVOID volatile *)&tree_templates[slot], value, NULL) == NULL;
#else
    TreeTemplate *expected = NULL;
    return __atomic_compare_exchange_n(&tree_templates[slot], &expected, value, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
#endif
}

#ifdef _MSC_VER
__declspec(noinline)
#else
__attribute__((noinline))
#endif
static TreeTemplate *make_tree_template(TreeShape shape, uint64_t key, uint64_t hash) {
    /* Keep the large miss-only scratch frame out of the cached-tree hot path. */
    int raw[1 + 4096 * 4];
    VegBuffer temp;
    vegbuf_init(&temp, raw, 4096);
    shape(&temp, 8, 8, 0, 1, 2, hash, -1024, 1024);
    TreeTemplate *entry = (TreeTemplate *)malloc(sizeof(TreeTemplate) + (size_t)temp.count * sizeof(TreeCell));
    if (!entry) return NULL;
    entry->key = key; entry->count = temp.count;
    for (int i = 0; i < temp.count; i++) {
        int *v = raw + 1 + i * 4;
        entry->cells[i] = (TreeCell){(int8_t)(v[0] - 8), (int8_t)v[1], (int8_t)(v[2] - 8), (uint8_t)v[3]};
    }
    return entry;
}

static int stamp_cached_tree(VegBuffer *vb, enum VegetationFamily family, int biome,
        int x, int z, int y, int log, int leaf, uint64_t hash, int bottom, int top) {
    /* Legacy canopies and snow-cap selection have boundary-specific clipping order. */
    if (y < bottom + 32 || y > top - 40) return 0;
    uint64_t params = 0, kind = 0;
    TreeShape shape = NULL;
    int roll = (int)floor_mod_u64(rotate_left_64(hash, 31), 100);
    switch (family) {
        case VEG_OAK:
            if (is_swamp_biome(biome)) {
                kind = 3; shape = place_swamp_oak_tree;
                params = 7 + floor_mod_u64(rotate_left_64(hash, 7), 4);
                const int rotations[] = {0, 13, 27, 41};
                for (int i = 0; i < 4; i++)
                    params |= (uint64_t)(floor_mod_u64(rotations[i] ? rotate_left_64(hash, rotations[i]) : hash, 100) < 50) << (8 + i);
            } else if (roll < (is_windswept_hills_biome(biome) ? 10 : is_windswept_biome(biome) ? 0 : 30)) {
                kind = 11; shape = place_fancy_oak_tree;
                int height = 7 + floor_mod_u64(hash, 8);
                int branches = 2 + floor_mod_u64(rotate_left_64(hash, 7), 3);
                params = (uint64_t)height | ((uint64_t)branches << 4);
                for (int i = 0; i < branches; i++) {
                    uint64_t at = 2 + floor_mod_u64(rotate_left_64(hash, 13 + i * 9), height - 2);
                    uint64_t dir = floor_mod_u64(rotate_left_64(hash, 21 + i * 11), 4);
                    params |= (at | (dir << 4)) << (8 + i * 6);
                }
            } else if (is_windswept_biome(biome)) {
                kind = 13; shape = place_windswept_oak_tree; params = 4 + floor_mod_u64(hash, 3);
            } else { kind = 1; shape = place_oak_tree; params = 6 + floor_mod_u64(hash, 3); }
            break;
        case VEG_BIRCH:
            if (roll < 25) { kind = 4; shape = place_super_birch_tree;
                params = 6 + floor_mod_u64(hash, 3) + floor_mod_u64(rotate_left_64(hash, 9), 6);
            } else { kind = 2; shape = place_birch_tree; params = 6 + floor_mod_u64(rotate_left_64(hash, 17), 3); }
            break;
        case VEG_SPRUCE:
            if (is_old_growth_taiga(biome) && roll < 35) {
                kind = 7; shape = place_mega_pine_tree;
                int height = 13 + floor_mod_u64(hash, 8);
                int crown = 13 + floor_mod_u64(rotate_left_64(hash, 7), 5);
                if (crown > height - 3) crown = height - 3;
                params = (uint64_t)height | ((uint64_t)crown << 5);
            } else if ((is_old_growth_taiga(biome) && roll < 65)
                    || (!is_old_growth_taiga(biome) && !is_windswept_hills_biome(biome) && roll < 40)) {
                kind = 6; shape = place_pine_tree;
                params = (7 + floor_mod_u64(hash, 5)) | (floor_mod_u64(rotate_left_64(hash, 9), 2) << 5);
            } else {
                int wind = is_windswept_hills_biome(biome);
                kind = wind ? 12 : 5; shape = wind ? place_windswept_spruce_tree : place_spruce_tree;
                params = ((wind ? 5 : 8) + floor_mod_u64(hash, 3) + floor_mod_u64(rotate_left_64(hash, 9), 2))
                    | (floor_mod_u64(rotate_left_64(hash, 5), 2) << 5)
                    | (floor_mod_u64(rotate_left_64(hash, 11), 3) << 6)
                    | (floor_mod_u64(rotate_left_64(hash, 23), 2) << 8);
                if (wind) params |= (uint64_t)floor_mod_u64(rotate_left_64(hash, 17), 2) << 9;
            }
            break;
        case VEG_DARK_OAK:
            /* The legacy shape changes trunk topology at a chunk edge. Preserve that path exactly. */
            if (x < 1 || x > 14 || z < 1 || z > 14) return 0;
            kind = 8; shape = place_dark_oak_tree;
            params = 6 + floor_mod_u64(hash, 3) + floor_mod_u64(rotate_left_64(hash, 9), 2);
            break;
        case VEG_ACACIA:
            kind = 9; shape = place_acacia_tree;
            params = (5 + floor_mod_u64(hash, 3) + floor_mod_u64(rotate_left_64(hash, 11), 3))
                | (floor_mod_u64(hash, 4) << 5);
            break;
        case VEG_JUNGLE:
            kind = 10; shape = place_jungle_tree;
            params = (6 + floor_mod_u64(hash, 8)) | (floor_mod_u64(rotate_left_64(hash, 9), 4) << 5)
                | (floor_mod_u64(rotate_left_64(hash, 23), 4) << 7);
            break;
        default: return 0; /* Cherry's per-leaf random holes retain the original deterministic function. */
    }
    if (kind <= 4 || kind == 13) for (int layer = 1; layer < 4; layer++)
        params |= (rotate_left_64(hash, 7 + layer * 13) & 1) << (16 + layer);
    uint64_t key = (kind << 56) | params;
    int slot = (int)((key ^ (key >> 23) ^ (key >> 47)) * 0x9e3779b97f4a7c15ULL >> 54);
    TreeTemplate *entry = NULL;
    for (int probe = 0; probe < 4; probe++, slot = (slot + 1) & 1023) {
        entry = tree_template_load(slot);
        if (entry && entry->key == key) break;
        if (entry) { entry = NULL; continue; }
        entry = make_tree_template(shape, key, hash);
        if (!entry) return 0;
        if (tree_template_install(slot, entry)) break;
        free(entry); entry = tree_template_load(slot);
        if (entry->key == key) break;
        entry = NULL;
    }
    if (!entry) return 0;
    for (int i = 0; i < entry->count; i++) {
        TreeCell cell = entry->cells[i];
        int by = y + cell.y;
        if (by >= bottom && by < top)
            vegbuf_emit(vb, x + cell.x, by, z + cell.z, cell.material == 1 ? log : leaf);
    }
    return 1;
}
