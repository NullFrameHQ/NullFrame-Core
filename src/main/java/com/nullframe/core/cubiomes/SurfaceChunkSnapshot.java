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

/**
 * Surface data for one 16 by 16 chunk, with optional packed block sections.
 * The constructor takes ownership of its arrays without copying. Callers must not
 * modify those arrays, including the arrays returned by the view methods.
 */
public final class SurfaceChunkSnapshot {

    private final int dimensionId;
    private final int chunkX;
    private final int chunkZ;
    private final int[] topHeights; // length = 16 * 16
    private final byte[] surfaceTypes; // length = 16 * 16
    private final int[] biomeIds; // length = 16 * 16
    private final NativeChunkData nativeChunkData;

    public SurfaceChunkSnapshot(int dimensionId,
                                int chunkX,
                                int chunkZ,
                                int[] topHeights,
                                byte[] surfaceTypes,
                                int[] biomeIds) {
        this(dimensionId, chunkX, chunkZ, topHeights, surfaceTypes, biomeIds, null);
    }

    public SurfaceChunkSnapshot(int dimensionId, int chunkX, int chunkZ, int[] topHeights,
                                byte[] surfaceTypes, int[] biomeIds, NativeChunkData nativeChunkData) {
        if (topHeights == null || topHeights.length != 16 * 16) {
            throw new IllegalArgumentException("topHeights must be a 16x16 (256) array");
        }
        if (surfaceTypes == null || surfaceTypes.length != 16 * 16) {
            throw new IllegalArgumentException("surfaceTypes must be a 16x16 (256) array");
        }
        if (biomeIds == null || biomeIds.length != 16 * 16) {
            throw new IllegalArgumentException("biomeIds must be a 16x16 (256) array");
        }
        this.dimensionId = dimensionId;
        this.chunkX = chunkX;
        this.chunkZ = chunkZ;
        // Take ownership of these arrays; copying here would duplicate every generated snapshot.


        this.topHeights = topHeights;
        this.surfaceTypes = surfaceTypes;
        this.biomeIds = biomeIds;
        this.nativeChunkData = nativeChunkData;
    }

    /** Present only when requested through the complete native chunk API. */
    public NativeChunkData nativeChunkData() { return nativeChunkData; }
    public long estimatedBytes() { return 2432L + (nativeChunkData == null ? 0 : nativeChunkData.estimatedBytes()); }

    public int getDimensionId() {
        return dimensionId;
    }

    public int getChunkX() {
        return chunkX;
    }

    public int getChunkZ() {
        return chunkZ;
    }

    /**
     * Returns the full 16x16 height array (defensive copy).
     */
    public int[] getTopHeights() {
        return topHeights.clone();
    }

    /**
     * Returns the full 16x16 surface type array (defensive copy).
     */
    public byte[] getSurfaceTypes() {
        return surfaceTypes.clone();
    }

    /**
     * Returns the full 16x16 biome-id array (defensive copy).
     */
    public int[] getBiomeIds() {
        return biomeIds.clone();
    }

    /**
     * Convenience accessor for the top Y at a specific local (x, z) within the chunk.
     *
     * @param x local X coordinate in [0, 15]
     * @param z local Z coordinate in [0, 15]
     */
    public int getTopHeight(int x, int z) {
        if (x < 0 || x >= 16 || z < 0 || z >= 16) {
            throw new IndexOutOfBoundsException("Chunk coordinates must be in [0, 15]");
        }
        return topHeights[z * 16 + x];
    }

    public byte getSurfaceType(int x, int z) {
        if (x < 0 || x >= 16 || z < 0 || z >= 16) {
            throw new IndexOutOfBoundsException("Chunk coordinates must be in [0, 15]");
        }
        return surfaceTypes[z * 16 + x];
    }

    public int getBiomeId(int x, int z) {
        if (x < 0 || x >= 16 || z < 0 || z >= 16) {
            throw new IndexOutOfBoundsException("Chunk coordinates must be in [0, 15]");
        }
        return biomeIds[z * 16 + x];
    }

    /** Borrowed height array. Read only; do not modify it. */
    public int[] topHeightsView() {
        return topHeights;
    }

    /** Borrowed surface-type array. Read only; do not modify it. */
    public byte[] surfaceTypesView() {
        return surfaceTypes;
    }

    /** Borrowed biome-ID array. Read only; do not modify it. */
    public int[] biomeIdsView() {
        return biomeIds;
    }
}
