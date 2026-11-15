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

#ifndef NULLFRAME_CORE_TYPES_H
#define NULLFRAME_CORE_TYPES_H

#include <stdint.h>
#include "biomes.h"
#include "cubiomes.h"
/* Shared data layout and material IDs; these must match the Java decoder. */
#if defined(_WIN32) || defined(_MSC_VER)
#define TLS_STORAGE __declspec(thread)
#else
#define TLS_STORAGE __thread
#endif
#define CHUNK_COLUMN_COUNT 256
#define PACKED_CHUNK_INTS (CHUNK_COLUMN_COUNT * 3)
#define DEFAULT_PLAINS_BIOME_ID plains

/* Surface types (must match Java SurfaceColumnType) */
enum SurfaceType
{
    SURFACE_GRASS = 0,
    SURFACE_SAND  = 1,
    SURFACE_STONE = 2,
    SURFACE_SNOW  = 3,
    SURFACE_WATER = 4,
};

/*
 * Core material IDs for vegetation. Java consumers map them to block states.
 * Keep these values consistent with NativeChunkData and the consumer's mapper.
 */
enum BlockType
{
    BLK_AIR = 0,
    BLK_OAK_LOG          = 10,
    BLK_BIRCH_LOG        = 11,
    BLK_SPRUCE_LOG       = 12,
    BLK_DARK_OAK_LOG     = 13,
    BLK_ACACIA_LOG       = 14,
    BLK_JUNGLE_LOG       = 15,
    BLK_CHERRY_LOG       = 16,
    BLK_OAK_LEAVES       = 20,
    BLK_BIRCH_LEAVES     = 21,
    BLK_SPRUCE_LEAVES    = 22,
    BLK_DARK_OAK_LEAVES  = 23,
    BLK_ACACIA_LEAVES    = 24,
    BLK_JUNGLE_LEAVES    = 25,
    BLK_CHERRY_LEAVES    = 26,
    BLK_DANDELION        = 30,
    BLK_POPPY            = 31,
    BLK_AZURE_BLUET      = 32,
    BLK_OXEYE_DAISY      = 33,
    BLK_CORNFLOWER       = 34,
    BLK_RED_TULIP        = 35,
    BLK_ORANGE_TULIP     = 36,
    BLK_WHITE_TULIP      = 37,
    BLK_PINK_TULIP       = 38,
    BLK_SHORT_GRASS      = 40,
    BLK_TALL_GRASS_LOWER = 41,
    BLK_TALL_GRASS_UPPER = 42,
    BLK_SUNFLOWER_LOWER  = 43,
    BLK_SUNFLOWER_UPPER  = 44,
    BLK_CACTUS           = 45,
    BLK_DEAD_BUSH        = 46,
    BLK_SNOW_LAYER       = 47,
    BLK_BROWN_MUSHROOM   = 48,
    BLK_RED_MUSHROOM     = 49,
    BLK_MUSHROOM_STEM    = 50,
    /* Five cap-face bits: west/east/north/south/up. Down is always false. */
    BLK_BROWN_CAP_BASE   = 96,
    BLK_RED_CAP_BASE     = 128,
};

enum VegetationFamily
{
    VEG_NONE = 0,
    VEG_OAK,
    VEG_BIRCH,
    VEG_DARK_OAK,
    VEG_SPRUCE,
    VEG_ACACIA,
    VEG_JUNGLE,
    VEG_CHERRY,
};

/* Single-chunk vegetation capacity. Each emitted block uses four ints, plus one count header. */
#define MAX_VEG_BLOCKS 10000
#define VEG_BUFFER_INTS (1 + MAX_VEG_BLOCKS * 4)

/* Per-chunk vegetation limit used by the reusable region output buffer. */
#define REGION_VEG_CAP_BLOCKS 4096

#endif
