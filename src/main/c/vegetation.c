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
#include <string.h>
#include "vegetation.h"
#include "tree_shapes.h"
#include "biome_ids.h"


static int passes_density(uint64_t seed, int world_x, int world_z, int biome_id, uint64_t salt, int density_bp) {
    uint64_t h = vegetation_hash(seed, world_x, world_z, biome_id, salt);
    return floor_mod_u64(h, VEG_DENSITY_DENOMINATOR) < density_bp;
}

/* 1 if a cactus would grow at this exact column (same density roll both
 * sides compute, so adjacent cacti give way deterministically). */
static int cactus_neighbor_would_grow(uint64_t seed, int world_x, int world_z, int biome_id, int cactus_bp) {
    return passes_density(seed, world_x, world_z, biome_id, 0xDEADBEEFCAFE1234ULL, cactus_bp);
}

static int vegetation_density_bp(int biome_id) {
    /* Tree candidate probability in basis points: 100 means one percent per column. */
    switch (biome_id) {
        case mountains: case mountain_edge: case gravelly_mountains: case modified_gravelly_mountains:
            return 4; /* 0.1 attempts/chunk, used for cross-chunk conflict checks. */
        case wooded_mountains:
            return 121; /* 3.1 attempts/chunk. Actual placement uses the weighted counts below. */
        /* Dense forest and jungle candidates; placement spacing can reject them. */
        case dark_forest: case 157:  /* dark_forest_hills */
            return 750;
        case jungle: case jungle_hills: case jungle_edge:
        case bamboo_jungle: case bamboo_jungle_hills:
            return 850;

        /* Forest and old-growth taiga candidates. */
        case forest: case wooded_hills: case flower_forest:
            return 520;
        case giant_tree_taiga: case giant_tree_taiga_hills:
        case 160: case 161:  /* giant_spruce_taiga, giant_spruce_taiga_hills */
            return 480;
        case snowy_tundra:  /* snowy_plains — very sparse spruce */
            return 10;

        /* Birch forest and taiga candidates. */
        case birch_forest: case birch_forest_hills:
        case 155: case 156:  /* tall_birch_forest, tall_birch_hills */
            return 300;
        case taiga: case taiga_hills:
        case snowy_taiga: case snowy_taiga_hills:
        case 159:  /* snowy_taiga_mountains */
            return 260;

        /* Savanna candidates. */
        case savanna: case savanna_plateau: case shattered_savanna:
        case 164:  /* shattered_savanna_plateau */
            return 180;

        /* Sparse swamp candidates. */
        case swamp: case swamp_hills: case mangrove_swamp:
            return 90;

        /* Plains have sparse tree candidates; sunflower plains and meadows use slightly more. */
        case plains:
            return 2;
        case 129:  /* sunflower_plains */
            return 3;
        case 177:  /* meadow */
            return 3;

        /* Cherry grove candidates. Spacing and border checks reduce the final tree count. */
        case 185:  /* cherry_grove */
            return 600;

        /* No trees */
        default:
            return 0;
    }
}

static enum VegetationFamily vegetation_family_for_biome(int biome_id) {
    switch (biome_id) {
        case mountains: case mountain_edge: case wooded_mountains:
        case gravelly_mountains: case modified_gravelly_mountains:
            return VEG_SPRUCE; /* Mixed with oak per candidate below. */
        case forest: case wooded_hills: case flower_forest:
        case plains: case 129:  /* sunflower_plains */
        case 177:   /* meadow */
        case swamp: case swamp_hills: case mangrove_swamp:
            return VEG_OAK;
        case birch_forest: case birch_forest_hills:
        case 155: case 156:
            return VEG_BIRCH;
        case dark_forest: case 157:
            return VEG_DARK_OAK;
        case taiga: case taiga_hills:
        case snowy_taiga: case snowy_taiga_hills:
        case 159:
        case giant_tree_taiga: case giant_tree_taiga_hills:
        case 160: case 161:
        case snowy_tundra:  /* snowy_plains */
            return VEG_SPRUCE;
        case savanna: case savanna_plateau: case shattered_savanna:
        case 164:
            return VEG_ACACIA;
        case jungle: case jungle_hills: case jungle_edge:
        case bamboo_jungle: case bamboo_jungle_hills:
            return VEG_JUNGLE;
        case 185:  /* cherry_grove */
            return VEG_CHERRY;
        default:
            return VEG_NONE;
    }
}

static int is_old_growth_taiga(int biome_id) {
    return biome_id == giant_tree_taiga
        || biome_id == giant_tree_taiga_hills
        || biome_id == 160
        || biome_id == 161;
}
static int is_snowy_plains_biome(int biome_id) { return biome_id == snowy_tundra; }

static int is_swamp_biome(int biome_id) {
    return biome_id == swamp || biome_id == swamp_hills || biome_id == mangrove_swamp;
}

static int is_sunflower_biome(int biome_id) { return biome_id == 129; }
static int is_flower_biome(int biome_id) {
    return is_windswept_hills_biome(biome_id) || biome_id == plains || biome_id == 129 || biome_id == 177 ||
           biome_id == birch_forest || biome_id == birch_forest_hills ||
           biome_id == flower_forest || biome_id == snowy_tundra;
}
/*
 * Number of grass placement attempts by biome.
 * Positions are scattered across the chunk rather than clustered like vanilla grass patches.
 */
static int grass_patch_tries(int biome_id) {
    switch (biome_id) {
        case mountains: case mountain_edge: case wooded_mountains:
        case gravelly_mountains: case modified_gravelly_mountains:
            return 28; /* One patch_grass_badlands attempt, using the existing patch approximation. */
        case plains: case 129: case 177:                          
        case 185:  /* cherry_grove — grass_block floor, keep it green */
            return 140;
        case forest: case wooded_hills: case flower_forest:        
        case birch_forest: case birch_forest_hills:
        case 155: case 156:
            return 56;
        case savanna: case savanna_plateau:                       
        case shattered_savanna: case 164:
            return 540;
        case taiga: case taiga_hills:                             
        case snowy_taiga: case snowy_taiga_hills: case 159:
            return 192;
        case jungle: case jungle_hills: case jungle_edge:         
        case bamboo_jungle: case bamboo_jungle_hills:
            return 660;
        case swamp: case swamp_hills: case mangrove_swamp:
        case dark_forest: case 157:
            return 28;
        case snowy_tundra:  /* snowy_plains — sparse grass */
            return 24;
        default:
            return 0;
    }
}

static int log_block_for_family(enum VegetationFamily family) {
    switch (family) {
        case VEG_BIRCH:    return BLK_BIRCH_LOG;
        case VEG_DARK_OAK: return BLK_DARK_OAK_LOG;
        case VEG_SPRUCE:   return BLK_SPRUCE_LOG;
        case VEG_ACACIA:   return BLK_ACACIA_LOG;
        case VEG_JUNGLE:   return BLK_JUNGLE_LOG;
        case VEG_CHERRY:   return BLK_CHERRY_LOG;
        default:           return BLK_OAK_LOG;
    }
}

static int leaf_block_for_family(enum VegetationFamily family) {
    switch (family) {
        case VEG_BIRCH:    return BLK_BIRCH_LEAVES;
        case VEG_DARK_OAK: return BLK_DARK_OAK_LEAVES;
        case VEG_SPRUCE:   return BLK_SPRUCE_LEAVES;
        case VEG_ACACIA:   return BLK_ACACIA_LEAVES;
        case VEG_JUNGLE:   return BLK_JUNGLE_LEAVES;
        case VEG_CHERRY:   return BLK_CHERRY_LEAVES;
        default:           return BLK_OAK_LEAVES;
    }
}

static int flower_block_for_hash(uint64_t hash) {
    switch (floor_mod_u64(hash, 9)) {
        case 0: return BLK_DANDELION;
        case 1: return BLK_POPPY;
        case 2: return BLK_AZURE_BLUET;
        case 3: return BLK_OXEYE_DAISY;
        case 4: return BLK_CORNFLOWER;
        case 5: return BLK_RED_TULIP;
        case 6: return BLK_ORANGE_TULIP;
        case 7: return BLK_WHITE_TULIP;
        default: return BLK_PINK_TULIP;
    }
}


/* Vegetation placement shared by individual requests and complete native chunks. */

/*
 * The VegBuffer is passed by value, but its data pointer belongs to the caller.
 * Generation writes the block count into that buffer before returning.
 */
#include "vegetation_templates.h"
#include "mushroom_features.h"

int generate_chunk_vegetation(uint64_t world_seed, int chunk_x, int chunk_z,
                                     const int *heights, const int *biomes,
                                     const int *surface_types,
                                     int bottom_y, int top_y_exclusive,
                                     VegBuffer vb) {
    int chunk_start_x = chunk_x * 16;
    int chunk_start_z = chunk_z * 16;
    unsigned char windswept_candidates[256] = {0};
    const int windswept_biomes[] = {mountains, mountain_edge, wooded_mountains, gravelly_mountains,
        modified_gravelly_mountains, shattered_savanna, shattered_savanna_plateau};
    for (int b = 0; b < 7; b++) {
        int biome = windswept_biomes[b];
        uint64_t hash = vegetation_hash(world_seed, chunk_x, chunk_z, biome, 0x73DA210B9F4E826CULL);
        int count = (biome == wooded_mountains ? 3 : biome == shattered_savanna || biome == shattered_savanna_plateau ? 2 : 0)
                + (floor_mod_u64(hash, 10) == 0);
        for (int n = 0; n < count; n++) {
            uint64_t at = vegetation_hash(world_seed, chunk_x, chunk_z, biome, hash + (uint64_t)n * 0x9E3779B97F4A7C15ULL);
            int i = floor_mod_u64(at, 256);
            if (biomes[i] == biome) windswept_candidates[i] = 1;
        }
    }

    /* Ground decorations (flowers, grass) */
    /* Place vegetation relative to the supplied terrain heights so plants meet the surface. */
    for (int local_z = 0; local_z < 16; local_z++) {
        for (int local_x = 0; local_x < 16; local_x++) {
            int idx = local_z * 16 + local_x;
            if (surface_types[idx] != SURFACE_GRASS && surface_types[idx] != SURFACE_SNOW) continue;

            int biome_id = biomes[idx];
            int world_x = chunk_start_x + local_x;
            int world_z = chunk_start_z + local_z;
            int terrain_y = heights[idx];
            int plant_y = terrain_y + 1;
            if (plant_y < bottom_y || plant_y >= top_y_exclusive) continue;

            /* Sunflower (sunflower_plains biome = 129) — 1 in 40 (2.5%) */
            if (is_sunflower_biome(biome_id) &&
                passes_density(world_seed, world_x, world_z, biome_id, 0x2B3C4D5E6F708192ULL, 250) &&
                plant_y + 1 < top_y_exclusive) {
                vegbuf_emit(&vb, local_x, plant_y, local_z, BLK_SUNFLOWER_LOWER);
                vegbuf_emit(&vb, local_x, plant_y + 1, local_z, BLK_SUNFLOWER_UPPER);
                continue;
            }

            /* Flower candidates: 12.5% in flower forest, 1.6% in other eligible biomes. */
            int flower_bp = (biome_id == flower_forest) ? 1250 : 160;
            if (is_flower_biome(biome_id) &&
                passes_density(world_seed, world_x, world_z, biome_id, 0x3141592653589793ULL, flower_bp)) {
                uint64_t fh = vegetation_hash(world_seed, world_x, world_z, biome_id, 0x3141592653589793ULL);
                vegbuf_emit(&vb, local_x, plant_y, local_z, flower_block_for_hash(fh));
                continue;
            }

        }
    }

    /*
     * Place desert and badlands decorations on sandy columns.
     * Cacti are desert-only; neighbouring cactus candidates reject each other.
     */
    for (int local_z = 0; local_z < 16; local_z++) {
        for (int local_x = 0; local_x < 16; local_x++) {
            int idx = local_z * 16 + local_x;
            if (surface_types[idx] != SURFACE_SAND) continue;
            int biome_id = biomes[idx];
            int is_desert = (biome_id == desert || biome_id == desert_hills || biome_id == desert_lakes);
            int is_mesa = isMesa(biome_id);
            if (!is_desert && !is_mesa) continue;

            int world_x = chunk_start_x + local_x;
            int world_z = chunk_start_z + local_z;
            int terrain_y = heights[idx];
            int plant_y = terrain_y + 1;
            if (plant_y < bottom_y || plant_y >= top_y_exclusive) continue;

            /* Dead bush candidates: 0.8% in deserts, 3% in badlands. */
            if (passes_density(world_seed, world_x, world_z, biome_id, 0x1A2B3C4D5E6F7081ULL,
                               is_mesa ? 300 : 80)) {
                vegbuf_emit(&vb, local_x, plant_y, local_z, BLK_DEAD_BUSH);
                continue;
            }

            /* Cactus candidates: desert-only, with a 0.25% chance per column before neighbour checks. */
            if (!is_desert) continue;
            int cactus_bp = 25;
            if (!passes_density(world_seed, world_x, world_z, biome_id, 0xDEADBEEFCAFE1234ULL, cactus_bp)) continue;
            /* vanilla rule: no cactus adjacent to another cactus */
            if (cactus_neighbor_would_grow(world_seed, world_x + 1, world_z, biome_id, cactus_bp) ||
                cactus_neighbor_would_grow(world_seed, world_x - 1, world_z, biome_id, cactus_bp) ||
                cactus_neighbor_would_grow(world_seed, world_x, world_z + 1, biome_id, cactus_bp) ||
                cactus_neighbor_would_grow(world_seed, world_x, world_z - 1, biome_id, cactus_bp)) {
                continue;
            }
            uint64_t ch = vegetation_hash(world_seed, world_x, world_z, biome_id, 0xDEADBEEFCAFE1234ULL);
            int cactus_h = 1 + (int) floor_mod_u64(rotate_left_64(ch, 31), 3);   /* 1-3 */
            for (int dy = 0; dy < cactus_h && plant_y + dy < top_y_exclusive; dy++) {
                vegbuf_emit(&vb, local_x, plant_y + dy, local_z, BLK_CACTUS);
            }
        }
    }

    /*
     * Scatter grass attempts using the biome at the chunk centre for the attempt count.
     * Each candidate is then checked against the biome and surface at its position.
     */
    int rep_biome = biomes[8 * 16 + 8];
    int grass_tries = grass_patch_tries(rep_biome);
    for (int t = 0; t < grass_tries; t++) {
        uint64_t th = vegetation_hash(world_seed, chunk_x, chunk_z, rep_biome,
                                      0x99BBCCDDEEFF0011ULL + (uint64_t) t * 0x9E3779B97F4A7C15ULL);
        int lx = floor_mod_u64(th, 16);
        int lz = floor_mod_u64(rotate_left_64(th, 23), 16);
        int idx = lz * 16 + lx;
        if (surface_types[idx] != SURFACE_GRASS) continue;
        if (grass_patch_tries(biomes[idx]) <= 0) continue;  /* biome filter at the position */
        int plant_y = heights[idx] + 1;
        if (plant_y < bottom_y || plant_y >= top_y_exclusive) continue;
        /* Mostly short grass; ~8% tall grass where it fits (PATCH_TALL_GRASS feel) */
        uint64_t gh = rotate_left_64(th, 37);
        if (floor_mod_u64(gh, 100) < 8 && plant_y + 1 < top_y_exclusive) {
            vegbuf_emit(&vb, lx, plant_y, lz, BLK_TALL_GRASS_LOWER);
            vegbuf_emit(&vb, lx, plant_y + 1, lz, BLK_TALL_GRASS_UPPER);
        } else {
            vegbuf_emit(&vb, lx, plant_y, lz, BLK_SHORT_GRASS);
        }
    }

    /* Trees */
    /* Track occupied columns so later tree candidates respect the selected canopy footprint. */
    int tree_spacing_grid[256];
    memset(tree_spacing_grid, 0, sizeof(tree_spacing_grid));

    for (int local_z = 0; local_z < 16; local_z++) {
        for (int local_x = 0; local_x < 16; local_x++) {
            int idx = local_z * 16 + local_x;
            if (surface_types[idx] != SURFACE_GRASS && surface_types[idx] != SURFACE_SNOW) continue;

            /* Check minimum spacing — skip if too close to another tree */
            if (tree_spacing_grid[idx]) continue;

            int biome_id = biomes[idx];
            int density_bp = vegetation_density_bp(biome_id);
            if (density_bp <= 0) continue;

            int world_x = chunk_start_x + local_x;
            int world_z = chunk_start_z + local_z;
            if (is_windswept_biome(biome_id) ? !windswept_candidates[idx]
                    : !passes_density(world_seed, world_x, world_z, biome_id, 0x3C79AC492BA7B653ULL, density_bp)) continue;

            enum VegetationFamily family = vegetation_family_for_biome(biome_id);
            if (family == VEG_NONE) continue;

            /*
             * Reject border trees when the same density hash predicts a nearby candidate
             * in a neighbouring chunk. This check estimates candidates using the current biome.
             */
            int footprint = 3;  /* r2 canopies (oak, birch, acacia, jungle, pine, spruce) */
            if (family == VEG_DARK_OAK || is_swamp_biome(biome_id) ||
                (family == VEG_SPRUCE && is_old_growth_taiga(biome_id))) {
                footprint = 4;  /* r3 canopies + 2x2 giants */
            }
            if (family == VEG_CHERRY) {
                footprint = 4;  /* r4 canopy — the widest single-trunk crown we place */
            }
            int conflict = 0;
            for (int fz = -footprint; fz <= footprint && !conflict; fz++) {
                for (int fx = -footprint; fx <= footprint && !conflict; fx++) {
                    int cwx = world_x + fx;
                    int cwz = world_z + fz;
                    int clx = floor_mod_u64((uint64_t) cwx, 16);
                    int clz = floor_mod_u64((uint64_t) cwz, 16);
                    int cchunk_x = (cwx - clx) / 16;
                    int cchunk_z = (cwz - clz) / 16;
                    if (cchunk_x == chunk_x && cchunk_z == chunk_z) continue;  /* handled by grid */
                    if (passes_density(world_seed, cwx, cwz, biome_id, 0x3C79AC492BA7B653ULL, density_bp)) {
                        conflict = 1;
                    }
                }
            }
            if (conflict) continue;

            int terrain_y = heights[idx];
            if (terrain_y + 1 >= top_y_exclusive) continue;

            uint64_t tree_hash = vegetation_hash(world_seed, world_x, world_z, biome_id, 0x5A17C9E3D4B6F1A3ULL);
            if (is_windswept_hills_biome(biome_id))
                family = floor_mod_u64(rotate_left_64(tree_hash, 19), 10000) < 6660 ? VEG_SPRUCE : VEG_OAK;
            else if (biome_id == shattered_savanna || biome_id == shattered_savanna_plateau)
                family = floor_mod_u64(rotate_left_64(tree_hash, 19), 10000) < 8000 ? VEG_ACACIA : VEG_OAK;
            int log_block = log_block_for_family(family);
            int leaf_block = leaf_block_for_family(family);

            /* Choose a deterministic tree variant. The available shapes and weights depend on the biome. */
            uint64_t vh = rotate_left_64(tree_hash, 31);
            int roll = floor_mod_u64(vh, 100);
            if (!vb.use_templates || !stamp_cached_tree(&vb, family, biome_id, local_x, local_z, terrain_y,
                    log_block, leaf_block, tree_hash, bottom_y, top_y_exclusive)) switch (family) {
                case VEG_SPRUCE:
                    if (is_old_growth_taiga(biome_id)) {
                        /* Old-growth mix: 35% mega pine, 30% pine and 35% spruce. */
                        if (roll < 35) {
                            place_mega_pine_tree(&vb, local_x, local_z, terrain_y, log_block, leaf_block,
                                                 tree_hash, bottom_y, top_y_exclusive);
                        } else if (roll < 65) {
                            place_pine_tree(&vb, local_x, local_z, terrain_y, log_block, leaf_block,
                                            tree_hash, bottom_y, top_y_exclusive);
                        } else {
                            place_spruce_tree(&vb, local_x, local_z, terrain_y, log_block, leaf_block,
                                    tree_hash, bottom_y, top_y_exclusive);
                        }
                    } else if (!is_windswept_hills_biome(biome_id) && roll < 40) {
                        place_pine_tree(&vb, local_x, local_z, terrain_y, log_block, leaf_block,
                                        tree_hash, bottom_y, top_y_exclusive);
                    } else {
                        (is_windswept_hills_biome(biome_id) ? place_windswept_spruce_tree : place_spruce_tree)
                                (&vb, local_x, local_z, terrain_y, log_block, leaf_block, tree_hash, bottom_y, top_y_exclusive);
                    }
                    break;
                case VEG_DARK_OAK:
                    place_dark_oak_tree(&vb, local_x, local_z, terrain_y, log_block, leaf_block,
                                        tree_hash, bottom_y, top_y_exclusive);
                    break;
                case VEG_ACACIA:
                    place_acacia_tree(&vb, local_x, local_z, terrain_y, log_block, leaf_block,
                                      tree_hash, bottom_y, top_y_exclusive);
                    break;
                case VEG_JUNGLE:
                    place_jungle_tree(&vb, local_x, local_z, terrain_y, log_block, leaf_block,
                                      tree_hash, bottom_y, top_y_exclusive);
                    break;
                case VEG_CHERRY:
                    place_cherry_tree(&vb, local_x, local_z, terrain_y, log_block, leaf_block,
                                      tree_hash, bottom_y, top_y_exclusive);
                    break;
                case VEG_BIRCH:
                    if (roll < 25) {
                        place_super_birch_tree(&vb, local_x, local_z, terrain_y, log_block, leaf_block,
                                               tree_hash, bottom_y, top_y_exclusive);
                    } else {
                        place_birch_tree(&vb, local_x, local_z, terrain_y, log_block, leaf_block,
                                         tree_hash, bottom_y, top_y_exclusive);
                    }
                    break;
                case VEG_OAK:
                    if (is_swamp_biome(biome_id)) {
                        place_swamp_oak_tree(&vb, local_x, local_z, terrain_y, log_block, leaf_block,
                                             tree_hash, bottom_y, top_y_exclusive);
                    } else if (roll < (is_windswept_hills_biome(biome_id) ? 10 : is_windswept_biome(biome_id) ? 0 : 30)) {
                        place_fancy_oak_tree(&vb, local_x, local_z, terrain_y, log_block, leaf_block,
                                             tree_hash, bottom_y, top_y_exclusive);
                    } else {
                        (is_windswept_biome(biome_id) ? place_windswept_oak_tree : place_oak_tree)
                                (&vb, local_x, local_z, terrain_y, log_block, leaf_block, tree_hash, bottom_y, top_y_exclusive);
                    }
                    break;
                default:
                    break;
            }

            /* Reserve a square around the trunk using this tree family's footprint radius. */
            int mark_radius = footprint;
            for (int dz = -mark_radius; dz <= mark_radius; dz++) {
                for (int dx = -mark_radius; dx <= mark_radius; dx++) {
                    int nx = local_x + dx;
                    int nz = local_z + dz;
                    if (nx >= 0 && nx < 16 && nz >= 0 && nz < 16) {
                        tree_spacing_grid[nz * 16 + nx] = 1;
                    }
                }
            }
        }
    }

    generate_mushrooms(world_seed, chunk_x, chunk_z, heights, biomes, surface_types, bottom_y, top_y_exclusive, &vb);

    /* In snowy biomes, add snow above the highest emitted leaf in each column where it fits. */
    int rep_biome_snow = biomes[8 * 16 + 8];
    if (isSnowy(rep_biome_snow)) {
        int top_leaf[256];
        for (int i = 0; i < 256; i++) top_leaf[i] = -1;
        for (int i = 0; i < vb.count; i++) {
            int *e = &vb.data[1 + i * 4];
            int type = e[3];
            if (type >= BLK_OAK_LEAVES && type <= BLK_JUNGLE_LEAVES) {
                int ci = e[2] * 16 + e[0];
                if (e[1] > top_leaf[ci]) top_leaf[ci] = e[1];
            }
        }
        for (int lz = 0; lz < 16; lz++) {
            for (int lx = 0; lx < 16; lx++) {
                int y = top_leaf[lz * 16 + lx];
                if (y < 0 || y + 1 >= top_y_exclusive) continue;
                vegbuf_emit(&vb, lx, y + 1, lz, BLK_SNOW_LAYER);
            }
        }
    }

    vegbuf_flush(&vb);
    return vb.count;
}

int *myelin_get_vegetation(long long seed,
                                int chunk_x, int chunk_z,
                                const int *heights,        /* 256 ints */
                                const int *biomes,         /* 256 ints */
                                const int *surface_types,  /* 256 ints */
                                int bottom_y,
                                int top_y_exclusive,
                                int *out_int_count) {
    if (heights == NULL || biomes == NULL || surface_types == NULL || out_int_count == NULL) {
        return NULL;
    }

    int *buffer = (int *) malloc(sizeof(int) * VEG_BUFFER_INTS);
    if (buffer == NULL) {
        *out_int_count = 1;
        return NULL;
    }

    VegBuffer vb;
    vegbuf_init(&vb, buffer, MAX_VEG_BLOCKS);
    int block_count = generate_chunk_vegetation((uint64_t) seed, chunk_x, chunk_z,
                                                heights, biomes, surface_types,
                                                bottom_y, top_y_exclusive, vb);
    *out_int_count = 1 + block_count * 4;
    return buffer;
}

/*
 * Region vegetation uses a reusable per-thread buffer.
 * Output is [chunkCount, (vegCount, localX, worldY, localZ, materialId repeated), ...].
 * The JNI caller copies the result before this thread reuses the buffer.
 */

static TLS_STORAGE int *s_region_veg_buffer = NULL;
static TLS_STORAGE int  s_region_veg_capacity = 0;

static int *region_veg_grow(int needed_ints) {
    if (needed_ints > s_region_veg_capacity) {
        int new_cap = s_region_veg_capacity > 0 ? s_region_veg_capacity : 65536;
        while (new_cap < needed_ints) new_cap *= 2;
        int *nb = (int *) realloc(s_region_veg_buffer, sizeof(int) * (size_t) new_cap);
        if (nb == NULL) return NULL;
        s_region_veg_buffer = nb;
        s_region_veg_capacity = new_cap;
    }
    return s_region_veg_buffer;
}

int *myelin_get_region_vegetation(long long seed, int count,
                                      const int *chunk_xs, const int *chunk_zs,
                                      const int *flat_heights, const int *flat_biomes,
                                      const int *flat_surface_types,
                                      int bottom_y, int top_y_exclusive,
                                      int *out_int_count) {
    if (count <= 0 || chunk_xs == NULL || chunk_zs == NULL || out_int_count == NULL) return NULL;
    if (flat_heights == NULL || flat_biomes == NULL || flat_surface_types == NULL) return NULL;

    int *buffer = region_veg_grow(1 + count);
    if (buffer == NULL) { *out_int_count = 1; return NULL; }
    buffer[0] = count;
    int offset = 1;
    for (int i = 0; i < count; i++) {
        /* Ensure room for one chunk's worst-case section before generating. */
        if (s_region_veg_capacity - offset < 1 + REGION_VEG_CAP_BLOCKS * 4) {
            buffer = region_veg_grow(offset + 1 + REGION_VEG_CAP_BLOCKS * 4);
            if (buffer == NULL) { *out_int_count = offset; return buffer; }
        }
        VegBuffer vb;
        vegbuf_init(&vb, buffer + offset, REGION_VEG_CAP_BLOCKS);
        int block_count = generate_chunk_vegetation((uint64_t) seed, chunk_xs[i], chunk_zs[i],
                                                    flat_heights + i * 256, flat_biomes + i * 256,
                                                    flat_surface_types + i * 256,
                                                    bottom_y, top_y_exclusive, vb);
        offset += 1 + block_count * 4;
    }
    *out_int_count = offset;
    return buffer;
}
