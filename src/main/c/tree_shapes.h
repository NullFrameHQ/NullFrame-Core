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

#ifndef NULLFRAME_TREE_SHAPES_H
#define NULLFRAME_TREE_SHAPES_H

#include "vegetation_internal.h"
void place_oak_tree(VegBuffer *vb, int local_x, int local_z, int terrain_y,
                           int log_block, int leaf_block, uint64_t hash,
                           int bottom_y, int top_y_exclusive);

void place_windswept_oak_tree(VegBuffer *vb, int x, int z, int y,
        int log, int leaf, uint64_t hash, int bottom, int top);

void place_birch_tree(VegBuffer *vb, int local_x, int local_z, int terrain_y,
                             int log_block, int leaf_block, uint64_t hash,
                             int bottom_y, int top_y_exclusive);

void place_swamp_oak_tree(VegBuffer *vb, int local_x, int local_z, int terrain_y,
                                 int log_block, int leaf_block, uint64_t hash,
                                 int bottom_y, int top_y_exclusive);

void place_spruce_tree(VegBuffer *vb, int x, int z, int y,
        int log, int leaf, uint64_t hash, int bottom, int top);

void place_windswept_spruce_tree(VegBuffer *vb, int x, int z, int y,
        int log, int leaf, uint64_t hash, int bottom, int top);

void place_fancy_oak_tree(VegBuffer *vb, int local_x, int local_z, int terrain_y,
                                 int log_block, int leaf_block, uint64_t hash,
                                 int bottom_y, int top_y_exclusive);

void place_super_birch_tree(VegBuffer *vb, int local_x, int local_z, int terrain_y,
                                   int log_block, int leaf_block, uint64_t hash,
                                   int bottom_y, int top_y_exclusive);

void place_pine_tree(VegBuffer *vb, int local_x, int local_z, int terrain_y,
                            int log_block, int leaf_block, uint64_t hash,
                            int bottom_y, int top_y_exclusive);

void place_mega_pine_tree(VegBuffer *vb, int local_x, int local_z, int terrain_y,
                                 int log_block, int leaf_block, uint64_t hash,
                                 int bottom_y, int top_y_exclusive);

void place_dark_oak_tree(VegBuffer *vb, int local_x, int local_z, int terrain_y,
                                int log_block, int leaf_block, uint64_t hash,
                                int bottom_y, int top_y_exclusive);

void place_acacia_tree(VegBuffer *vb, int local_x, int local_z, int terrain_y,
                              int log_block, int leaf_block, uint64_t hash,
                              int bottom_y, int top_y_exclusive);

void place_jungle_tree(VegBuffer *vb, int local_x, int local_z, int terrain_y,
                              int log_block, int leaf_block, uint64_t hash,
                              int bottom_y, int top_y_exclusive);

void place_cherry_tree(VegBuffer *vb, int local_x, int local_z, int terrain_y,
                              int log_block, int leaf_block, uint64_t hash,
                              int bottom_y, int top_y_exclusive);

#endif
