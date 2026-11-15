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

package com.nullframe.core.cubiomes;

/** Surface category codes shared by the native generator and Java consumers. */
public final class SurfaceColumnType {
    public static final byte GRASS = 0;
    public static final byte SAND = 1;
    public static final byte STONE = 2;
    public static final byte SNOW = 3;
    public static final byte WATER = 4;

    private SurfaceColumnType() {
    }

    public static boolean isWater(byte type) {
        return type == WATER;
    }
}
