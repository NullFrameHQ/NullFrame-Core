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

#ifndef MYELIN_CUBIOMES_H
#define MYELIN_CUBIOMES_H

#ifdef __cplusplus
extern "C" {
#endif

#define CB_DIM_OVERWORLD 0
#define CB_DIM_NETHER   -1
#define CB_DIM_END       1
#define CB_DIM_CUSTOM   99

int  myelin_get_terrain_height(long long seed, int dimension_id, int x, int z);
void myelin_get_heightmap(long long seed, int dimension_id,
                               int chunk_x, int chunk_z, int *out_heights);

#ifdef __cplusplus
}
#endif

#endif /* MYELIN_CUBIOMES_H */
