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

/** Settings and vertical bounds for native Surface chunk construction. */
public record NativeChunkOptions(int bottomY, int topYExclusive, boolean fillBottom, boolean vegetation,
                                 int surfaceChunkDepth) {
    /** Uses adaptive surface depth for callers that omit the explicit depth setting. */
    public NativeChunkOptions(int bottomY, int topYExclusive, boolean fillBottom, boolean vegetation) {
        this(bottomY, topYExclusive, fillBottom, vegetation, -1);
    }

    public NativeChunkOptions {
        if ((bottomY & 15) != 0 || (topYExclusive & 15) != 0
                || topYExclusive <= bottomY || (long) topYExclusive - bottomY > 4096
                || bottomY < -1_000_000 || topYExclusive > 1_000_000)
            throw new IllegalArgumentException("Native chunk bounds must be section-aligned, positive, and <=4096 blocks high");
        if (surfaceChunkDepth < -1 || surfaceChunkDepth > 4096)
            throw new IllegalArgumentException("Surface depth must be 0..4096, or -1 for legacy adaptive shells");
    }
}
