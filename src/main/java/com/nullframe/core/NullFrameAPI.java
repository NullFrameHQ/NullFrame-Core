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

package com.nullframe.core;

import com.nullframe.core.cubiomes.Cubiomes;
import com.nullframe.core.cubiomes.SurfaceChunkSnapshot;

/**
 * Public entry points for Cubiomes-backed surface terrain.
 *
 * <p>Requests run on the calling thread and return data without Minecraft objects.
 * Callers provide their own workers and decide how to cache or install the results.
 * Results describe approximate surface terrain.</p>
 *
 * <p>Native generator state and transport buffers are reused per thread. Quality
 * setters affect shared native settings, so they are not independent per request.</p>
 *
 * <pre>{@code
 * SurfaceChunkSnapshot chunk = NullFrameAPI.getSnapshot(
 *         seed, Cubiomes.DIMENSION_OVERWORLD, chunkX, chunkZ, 4);
 * int height = chunk.getTopHeight(8, 8);
 * }</pre>
 *
 * <p>Use {@link #cubiomes()} on the singleton for the lower-level API.</p>
 */
public final class NullFrameAPI {
    /** Complete native surface sections, with vegetation explicitly selected by the caller. */
    public static SurfaceChunkSnapshot getBuiltChunk(long seed, int dimension, int x, int z, int step,
            com.nullframe.core.cubiomes.NativeChunkOptions options) {
        return INSTANCE.cubiomes.getBuiltSurfaceChunk(seed, dimension, x, z, step, options);
    }

    /** Generates packed Surface chunks for the selected positions. A null mask selects all positions. */
    public static SurfaceChunkSnapshot[] getBuiltRegion(long seed, int dimension, int x, int z,
            int width, int height, int step, byte[] mask, boolean regionMode,
            com.nullframe.core.cubiomes.NativeChunkOptions options) {
        return INSTANCE.cubiomes.getBuiltSurfaceRegion(seed, dimension, x, z, width, height, step, mask, regionMode, options);
    }

    /** Per-chunk delivery from a native strip using a reusable direct buffer on the calling worker. */
    public static void streamBuiltRegion(long seed, int dimension, int x, int z,
            int width, int height, int step, byte[] mask, boolean regionMode,
            com.nullframe.core.cubiomes.NativeChunkOptions options,
            java.util.function.BiConsumer<Integer, SurfaceChunkSnapshot> consumer) {
        INSTANCE.cubiomes.streamBuiltSurfaceRegion(seed, dimension, x, z, width, height, step,
                mask, regionMode, options, consumer);
    }

    private static final NullFrameAPI INSTANCE = new NullFrameAPI();

    private final Cubiomes cubiomes;

    private NullFrameAPI() {
        this.cubiomes = new Cubiomes();
    }

    /** Returns the singleton (for advanced access via {@link #cubiomes()}). */
    public static NullFrameAPI getInstance() {
        return INSTANCE;
    }

    /** Access to the Cubiomes engine for advanced/direct calls. */
    public Cubiomes cubiomes() {
        return cubiomes;
    }


    /** True if the native Cubiomes library loaded successfully. */
    public static boolean isNativeAvailable() {
        return Cubiomes.isNativeAvailable();
    }

    /**
     * Generates one Surface snapshot using a reusable direct buffer when available.
     * The call blocks until the result is ready.
     *
     * @param seed world seed
     * @param dimensionId 0 for Overworld, -1 for Nether, 1 for End
     * @param chunkX chunk X coordinate
     * @param chunkZ chunk Z coordinate
     * @param samplingStep terrain sample spacing: 1, 2, 4, 8, 16, 32, 64 or 128
     * @return heights, surface types and biome IDs; a flat fallback if native loading failed
     */
    public static SurfaceChunkSnapshot getSnapshot(long seed, int dimensionId,
                                                   int chunkX, int chunkZ, int samplingStep) {
        return INSTANCE.cubiomes.getSurfaceChunkSnapshotDirect(seed, dimensionId, chunkX, chunkZ, samplingStep);
    }

    /**
     * Generates a rectangular region using the requested sample spacing.
     * Falls back to individual chunk requests if the region method is unavailable.
     *
     * @return one snapshot per chunk in row-major order
     */
    public static SurfaceChunkSnapshot[] getRegionSnapshots(long seed, int dimensionId,
                                                            int originX, int originZ,
                                                            int width, int height, int samplingStep) {
        return INSTANCE.cubiomes.getSurfaceRegionSnapshots(seed, dimensionId,
                originX, originZ, width, height, samplingStep);
    }

    /** Gets the terrain height at one block position. */
    public static int getTerrainHeight(long seed, int dimensionId, int blockX, int blockZ) {
        return INSTANCE.cubiomes.getTerrainHeight(seed, dimensionId, blockX, blockZ);
    }

    /**
     * Generates vegetation as {@code [count, x, y, z, materialId, ...]}.
     * X and Z are chunk-local, Y is a world coordinate, and material IDs use Core's
     * vegetation constants. Returns null if native generation is unavailable.
     */
    public static int[] getVegetation(long seed, int chunkX, int chunkZ,
                                      int[] heights, int[] biomes, int[] surfaceTypes,
                                      int bottomY, int topYExclusive) {
        return INSTANCE.cubiomes.getVegetation(seed, chunkX, chunkZ,
                heights, biomes, surfaceTypes, bottomY, topYExclusive);
    }

    /**
     * Returns the current-version resource name (e.g. {@code "savanna"}) for a
     * cubiomes biome id, or null if unknown. Use with the game's biome registry.
     */
    public static String getBiomeName(int biomeId) {
        return Cubiomes.getBiomeName(biomeId);
    }

    /**
     * Generates surface data and bakes region quads in one native call.
     * Each quad contains four 28-byte vertices: position, UV, ABGR colour, texture
     * index and flags. See {@code quad_baker.h} for the layout.
     *
     * <p>The returned buffer is reused by the next bake on this thread. Consume or
     * copy it before making another request.</p>
     */
    public static Cubiomes.QuadBakeResult bakeRegionQuads(long seed, int dimensionId,
                                                          int regionChunkX, int regionChunkZ,
                                                          int regionWidth, int regionHeight,
                                                          int samplingStep,
                                                          int centerBlockX, int centerBlockZ,
                                                          int[] tierBlocks, int[] tierMaxDists) {
        return Cubiomes.bakeRegionQuads(seed, dimensionId,
                regionChunkX, regionChunkZ, regionWidth, regionHeight,
                samplingStep, centerBlockX, centerBlockZ,
                tierBlocks, tierMaxDists);
    }
}
