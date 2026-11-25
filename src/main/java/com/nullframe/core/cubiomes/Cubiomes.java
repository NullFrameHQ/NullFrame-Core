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

import com.nullframe.core.StepTimings;
import org.apache.logging.log4j.LogManager;
import org.apache.logging.log4j.Logger;

import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.DirectoryStream;
import java.nio.file.FileSystems;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.nio.file.attribute.PosixFilePermission;
import java.nio.file.attribute.PosixFilePermissions;
import java.nio.file.attribute.UserPrincipal;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Arrays;
import java.util.HexFormat;
import java.util.Locale;
import java.util.Set;

public class Cubiomes {
    private static native int nativeWriteCompactColumnsV1(long seed, int minBlockX, int minBlockZ,
            int width, int height, int spacing, int bottomY, int topY, boolean vegetation, ByteBuffer output);

    /** Caller-owned scratch buffer for compact column requests. Reuse it between requests. */
    public static final class CompactColumnWorkspace {
        private ByteBuffer buffer = ByteBuffer.allocateDirect(1 << 20).order(ByteOrder.nativeOrder());
        private boolean busy;
        public int capacityBytes() { return buffer.capacity(); }
    }

    /**
     * Samples Overworld columns independently of the global terrain and biome steps.
     * Spacing selects which world columns are returned. With vegetation enabled, C builds
     * each touched chunk at full resolution before selecting its columns. Spacing above one
     * can miss trees between sampled columns.
     *
     * The result borrows this workspace's buffer and is valid only during the callback.
     */
    public void withCompactSurfaceColumns(long seed, int minBlockX, int minBlockZ,
            int width, int height, int spacing, int bottomY, int topY, boolean vegetation,
            CompactColumnWorkspace workspace, java.util.function.Consumer<CompactTerrainColumns> consumer) {
        java.util.Objects.requireNonNull(workspace); java.util.Objects.requireNonNull(consumer);
        if (width < 1 || height < 1 || width > 64 || height > 64
                || spacing < 1 || spacing > 512 || (spacing & (spacing - 1)) != 0
                || bottomY < -1_000_000 || topY > 1_000_000 || topY <= bottomY || (long)topY - bottomY > 4096
                || minBlockX < Integer.MIN_VALUE + 32 || minBlockZ < Integer.MIN_VALUE + 32
                || (long)minBlockX + (width - 1L) * spacing > Integer.MAX_VALUE - 32
                || (long)minBlockZ + (height - 1L) * spacing > Integer.MAX_VALUE - 32)
            throw new IllegalArgumentException("Invalid compact column grid, spacing, or height bounds");
        if (!nativeAvailable) throw new IllegalStateException("Compact terrain requires native NullFrame Core");
        synchronized (workspace) {
            if (workspace.busy) throw new IllegalStateException("Compact terrain workspace is already in use");
            workspace.busy = true;
            CompactTerrainColumns result = null;
            try {
                int bytes = nativeWriteCompactColumnsV1(seed, minBlockX, minBlockZ, width, height, spacing,
                        bottomY, topY, vegetation, workspace.buffer);
                if (bytes < 0) {
                    int maximum = Math.addExact(24, Math.multiplyExact(width * height, 12 + (topY - bottomY) * 8));
                    if (bytes == Integer.MIN_VALUE || -bytes > maximum)
                        throw new IllegalStateException("Invalid compact buffer size from native library");
                    workspace.buffer = ByteBuffer.allocateDirect(-bytes).order(ByteOrder.nativeOrder());
                    bytes = nativeWriteCompactColumnsV1(seed, minBlockX, minBlockZ, width, height, spacing,
                            bottomY, topY, vegetation, workspace.buffer);
                }
                if (bytes < 24 || bytes > workspace.buffer.capacity())
                    throw new IllegalStateException("Incomplete native compact terrain result");
                workspace.buffer.clear().limit(bytes);
                result = new CompactTerrainColumns(workspace.buffer, width, height, bottomY, topY);
                consumer.accept(result);
            } finally {
                if (result != null) result.invalidate();
                workspace.busy = false;
            }
        }
    }

    private static native void nativeStreamBuiltSurfaceRegionV2(long seed, int dimension, int x, int z,
            int width, int height, int samplingStep, byte[] mask, boolean regionMode,
            int bottomY, int topYExclusive, boolean fillBottom, boolean vegetation, int surfaceChunkDepth,
            ByteBuffer output, BuiltChunkStream receiver);

    private static final ThreadLocal<BuiltChunkStream> BUILT_CHUNK_STREAM = new ThreadLocal<>();

    /** One bounded transport buffer per calling worker, independent of cached snapshot storage. */
    private static final class BuiltChunkStream {
        final ByteBuffer output = ByteBuffer.allocateDirect(1 << 20).order(ByteOrder.nativeOrder());
        boolean busy;
        int dimension, x, z, width;
        long nativeStarted;
        byte[] mask;
        boolean[] delivered;
        NativeChunkOptions options;
        java.util.function.BiConsumer<Integer, SurfaceChunkSnapshot> consumer;

        // Called synchronously by JNI on this same worker before output is overwritten.
        @SuppressWarnings("unused")
        private void onChunk(int index, int bytes) {
            StepTimings.record(StepTimings.Step.NATIVE_CHUNK, nativeStarted);
            if (index < 0 || index >= delivered.length || delivered[index]
                    || (mask != null && mask[index] == 0) || bytes < 3096 || bytes > output.capacity())
                throw new IllegalStateException("Invalid native streamed chunk");
            output.clear().limit(bytes);
            long unpacked = System.nanoTime();
            SurfaceChunkSnapshot value;
            try {
                value = NativeChunkData.decode(dimension, x + index % width, z + index / width, options, output);
            } finally {
                StepTimings.record(StepTimings.Step.UNPACK, unpacked);
            }
            delivered[index] = true;
            consumer.accept(index, value);
            nativeStarted = System.nanoTime();
        }
    }

    /**
     * Generates a strip but delivers each complete chunk immediately on the calling thread.
     * Terrain, vegetation and palette packing remain native. Results own their storage, so the
     * worker can overwrite its direct buffer immediately after the consumer returns. A throwing
     * consumer stops the native loop; chunks already delivered remain valid. Nested streams on
     * the same thread are rejected instead of sharing an in-use buffer.
     */
    public void streamBuiltSurfaceRegion(long seed, int dimension, int x, int z,
            int width, int height, int samplingStep, byte[] mask, boolean regionMode, NativeChunkOptions options,
            java.util.function.BiConsumer<Integer, SurfaceChunkSnapshot> consumer) {
        java.util.Objects.requireNonNull(options, "options");
        java.util.Objects.requireNonNull(consumer, "consumer");
        if (width < 1 || height < 1 || width > 32 || height > 32 || width * height > 256
                || samplingStep < 1 || samplingStep > 128 || (samplingStep & (samplingStep - 1)) != 0
                || (mask != null && mask.length != width * height)
                || (long) x + width - 1 > Integer.MAX_VALUE || (long) z + height - 1 > Integer.MAX_VALUE)
            throw new IllegalArgumentException("Invalid complete surface region dimensions, step, or mask");
        if (!nativeAvailable) throw new IllegalStateException("Complete surface chunks require native NullFrame Core");
        BuiltChunkStream stream = BUILT_CHUNK_STREAM.get();
        if (stream == null) {
            stream = new BuiltChunkStream();
            BUILT_CHUNK_STREAM.set(stream);
        }
        if (stream.busy) throw new IllegalStateException("A native chunk stream is already using this worker's buffer");
        stream.busy = true;
        try {
            stream.dimension = dimension; stream.x = x; stream.z = z; stream.width = width;
            stream.options = options; stream.consumer = consumer;
            stream.mask = mask == null ? null : mask.clone();
            stream.delivered = new boolean[width * height];
            stream.nativeStarted = System.nanoTime();
            nativeStreamBuiltSurfaceRegionV2(seed, dimension, x, z, width, height, samplingStep,
                    stream.mask, regionMode, options.bottomY(), options.topYExclusive(), options.fillBottom(),
                    options.vegetation(), options.surfaceChunkDepth(), stream.output, stream);
            for (int i = 0; i < stream.delivered.length; i++)
                if ((stream.mask == null || stream.mask[i] != 0) && !stream.delivered[i])
                    throw new IllegalStateException("Incomplete native chunk stream");
        } finally {
            stream.consumer = null; stream.options = null; stream.mask = null; stream.delivered = null;
            stream.busy = false;
        }
    }

    /** Release this worker's reference at thread shutdown; direct memory is reclaimed by the JVM. */
    public static void releaseBuiltChunkBuffer() {
        BuiltChunkStream stream = BUILT_CHUNK_STREAM.get();
        if (stream != null && stream.busy) throw new IllegalStateException("Cannot release an active native chunk stream");
        BUILT_CHUNK_STREAM.remove();
    }

    private static native byte[][] nativeGetBuiltSurfaceRegionV2(long seed, int dimension, int x, int z,
            int width, int height, int samplingStep, byte[] mask, boolean regionMode,
            int bottomY, int topYExclusive, boolean fillBottom, boolean vegetation, int surfaceChunkDepth);
    private static native byte[] nativeBuildSurfaceSnapshotV2(long seed, int dimension, int x, int z,
            int[] columns, int bottomY, int topYExclusive, boolean fillBottom, boolean vegetation, int surfaceChunkDepth);

    /** Builds complete sections from an existing heightmap, useful for custom terrain producers. */
    public SurfaceChunkSnapshot buildSurfaceSnapshot(long seed, SurfaceChunkSnapshot snapshot, NativeChunkOptions options) {
        java.util.Objects.requireNonNull(snapshot, "snapshot");
        java.util.Objects.requireNonNull(options, "options");
        if (!nativeAvailable) throw new IllegalStateException("Complete surface chunks require native NullFrame Core");
        int[] columns = new int[768];
        System.arraycopy(snapshot.topHeightsView(), 0, columns, 0, 256);
        for (int i = 0; i < 256; i++) columns[256 + i] = snapshot.surfaceTypesView()[i];
        System.arraycopy(snapshot.biomeIdsView(), 0, columns, 512, 256);
        return NativeChunkData.decode(snapshot.getDimensionId(), snapshot.getChunkX(), snapshot.getChunkZ(), options,
                nativeBuildSurfaceSnapshotV2(seed, snapshot.getDimensionId(), snapshot.getChunkX(), snapshot.getChunkZ(),
                        columns, options.bottomY(), options.topYExclusive(), options.fillBottom(), options.vegetation(), options.surfaceChunkDepth()));
    }

    /**
     * Complete surface blocks, including optional vegetation, generated in one native job.
     * Returned palettes use stable NullFrame block IDs, never Minecraft runtime registry IDs.
     * Each result owns its arrays and can safely outlive/reuse the native worker's scratch memory.
     * No fallback hides an outdated native binary: update Core and its native library together.
     */
    public SurfaceChunkSnapshot[] getBuiltSurfaceRegion(long seed, int dimension, int x, int z,
            int width, int height, int samplingStep, byte[] mask, boolean regionMode, NativeChunkOptions options) {
        java.util.Objects.requireNonNull(options, "options");
        if (width < 1 || height < 1 || width > 32 || height > 32 || width * height > 256
                || samplingStep < 1 || samplingStep > 128 || (samplingStep & (samplingStep - 1)) != 0
                || (mask != null && mask.length != width * height)
                || (long) x + width - 1 > Integer.MAX_VALUE || (long) z + height - 1 > Integer.MAX_VALUE)
            throw new IllegalArgumentException("Invalid complete surface region dimensions, step, or mask");
        if (!nativeAvailable) throw new IllegalStateException("Complete surface chunks require native NullFrame Core");
        long started = System.nanoTime();
        byte[][] data = nativeGetBuiltSurfaceRegionV2(seed, dimension, x, z, width, height, samplingStep,
                mask, regionMode, options.bottomY(), options.topYExclusive(), options.fillBottom(), options.vegetation(), options.surfaceChunkDepth());
        StepTimings.record(StepTimings.Step.NATIVE_CHUNK, started);
        if (data == null || data.length != width * height) throw new IllegalStateException("Incomplete native chunk region");
        started = System.nanoTime();
        SurfaceChunkSnapshot[] result = new SurfaceChunkSnapshot[data.length];
        for (int i = 0; i < result.length; i++) {
            if (mask != null && mask[i] == 0) {
                if (data[i] != null) throw new IllegalStateException("Native compute mask ignored");
            } else result[i] = NativeChunkData.decode(dimension, x + i % width, z + i / width, options, data[i]);
        }
        StepTimings.record(StepTimings.Step.UNPACK, started);
        return result;
    }

    public SurfaceChunkSnapshot getBuiltSurfaceChunk(long seed, int dimension, int x, int z,
            int samplingStep, NativeChunkOptions options) {
        return getBuiltSurfaceRegion(seed, dimension, x, z, 1, 1, samplingStep, null, false, options)[0];
    }
    private static final Logger LOGGER = LogManager.getLogger("nullframe-core");

    public static final int DIMENSION_OVERWORLD = 0;
    public static final int DIMENSION_NETHER    = -1;
    public static final int DIMENSION_END       = 1;
    public static final int DIMENSION_CUSTOM    = 99;
    private static final int CHUNK_COLUMN_COUNT = 16 * 16;
    private static final int PACKED_CHUNK_INTS = CHUNK_COLUMN_COUNT * 3;
    private static final int MAX_REGION_SIZE = 8;
    private static final int DEFAULT_PLAINS_BIOME_ID = 1;

    private static volatile boolean nativeAvailable = false;
    private static volatile boolean regionNativeMethodAvailable = true;

    static {
        /* Load on both the client and dedicated server; single player also needs the native library. */
        try {
            String resourcePath = nativeResourcePathForCurrentOs();
            if (resourcePath == null) {
                LOGGER.warn("[NullFrameCore] Unsupported OS — native Cubiomes disabled, " +
                        "using flat y=64 fallback.");
            } else {
                Path libPath = extractNativeLibrary(resourcePath);
                System.load(libPath.toAbsolutePath().toString());
                nativeAvailable = true;
                LOGGER.info("[NullFrameCore] Native Cubiomes bridge loaded from {}.",
                        resourcePath);
            }
        } catch (Throwable t) {
            nativeAvailable = false;
            LOGGER.error("[NullFrameCore] Failed to load native Cubiomes bridge — " +
                    "falling back to flat y=64 for all surface chunks. " +
                    "Did you compile the C library and put it in resources/native/?", t);
        }
    }

    private static native int   nativeGetTerrainHeight(long seed, int dimensionId,
                                                       int blockX, int blockZ);
    private static native int[] nativeGetSurfaceChunkData(long seed, int dimensionId,
                                                          int chunkX, int chunkZ);
    private static native int[] nativeGetRegionSurfaceData(long seed, int dimensionId,
                                                           int regionChunkX, int regionChunkZ,
                                                           int regionWidth, int regionHeight);
    private static native int[] nativeGetVegetation(long seed, int chunkX, int chunkZ,
                                                    int[] heights, int[] biomes, int[] surfaceTypes,
                                                    int bottomY, int topYExclusive);
    /** Batched vegetation: one JNI call for many chunks (region strip veg pass). */
    private static native int[] nativeGetRegionVegetation(long seed,
                                                          int[] chunkXs, int[] chunkZs,
                                                          int[] flatHeights, int[] flatBiomes, int[] flatSurfaceTypes,
                                                          int bottomY, int topYExclusive);
    /** Samples a grid of terrain columns at the requested block spacing. */
    private static native void nativeGetLodGrid(long seed, int cellSizeBlocks,
                                                int minBlockX, int minBlockZ,
                                                int gridWidth, int gridHeight,
                                                int samplingStep,
                                                int[] outHeights, int[] outBiomes, int[] outTypes);
    /** Quad-baker: upper bound on quads for a region + tier table (pure grid math, no gen). */
    private static native int nativeQuadBakeCapacity(int regionChunkX, int regionChunkZ,
                                                     int regionWidth, int regionHeight,
                                                     int centerBlockX, int centerBlockZ,
                                                     int[] tierBlocks, int[] tierMaxDists);
    /** Quad-baker: generate region + bake canonical quads in ONE C pass into a direct buffer.
     * Returns quad count, or -2 if the buffer is too small (caller sized with
     * {@link #nativeQuadBakeCapacity}). QbQuad records are 112 bytes each. */
    private static native int nativeBakeRegionQuads(long seed, int dimensionId,
                                                    int regionChunkX, int regionChunkZ,
                                                    int regionWidth, int regionHeight,
                                                    int samplingStep,
                                                    int centerBlockX, int centerBlockZ,
                                                    int[] tierBlocks, int[] tierMaxDists,
                                                    ByteBuffer outBuffer);

    /**
     * Fills a grid of heights, biome IDs and surface types, starting at the given block
     * coordinates. Each output array needs {@code gridWidth * gridHeight} entries.
     * Returns false if native loading failed, inputs are missing, or the JNI method is absent.
     */
    public static boolean getLodGrid(long seed, int cellSizeBlocks,
                                     int minBlockX, int minBlockZ,
                                     int gridWidth, int gridHeight,
                                     int samplingStep,
                                     int[] outHeights, int[] outBiomes, int[] outTypes) {
        if (!nativeAvailable) return false;
        if (outHeights == null || outBiomes == null || outTypes == null) return false;
        if (outHeights.length != gridWidth * gridHeight) return false;
        try {
            nativeGetLodGrid(seed, cellSizeBlocks, minBlockX, minBlockZ,
                    gridWidth, gridHeight, samplingStep, outHeights, outBiomes, outTypes);
            return true;
        } catch (UnsatisfiedLinkError e) {
            LOGGER.warn("[NullFrameCore] nativeGetLodGrid unavailable — DH coarse LODs disabled.");
            return false;
        }
    }

    // Direct-buffer native methods.
    // C writes into off-heap memory; Java then copies the values into snapshot arrays.
    private static native void nativeGetSurfaceChunkDataDirect(long seed, int dimensionId,
                                                               int chunkX, int chunkZ,
                                                               int samplingStep,
                                                               ByteBuffer directBuffer);
    private static native void nativeGetRegionSurfaceDataDirect(long seed, int dimensionId,
                                                                int regionChunkX, int regionChunkZ,
                                                                int regionWidth, int regionHeight,
                                                                ByteBuffer directBuffer);
    /**
     * Generates a region at the requested sampling step in one native call.
     * Each chunk uses [256 heights][256 surface types][256 biome IDs], in row-major order.
     */
    private static native void nativeGetSurfaceRegionDataDirect(long seed, int dimensionId,
                                                                int regionChunkX, int regionChunkZ,
                                                                int regionWidth, int regionHeight,
                                                                int samplingStep,
                                                                ByteBuffer directBuffer);
    /** Same as above but only computes chunks where mask[idx] != 0 (skips cached/pending holes). */
    private static native void nativeGetSurfaceRegionDataDirectMasked(long seed, int dimensionId,
                                                                      int regionChunkX, int regionChunkZ,
                                                                      int regionWidth, int regionHeight,
                                                                      int samplingStep,
                                                                      byte[] mask, ByteBuffer directBuffer);

    /** Reusable per-thread transport for one chunk: 768 ints, or 3,072 bytes. */
    private static final ThreadLocal<ByteBuffer> CHUNK_BUFFER_TL = ThreadLocal.withInitial(() ->
            ByteBuffer.allocateDirect(PACKED_CHUNK_INTS * 4)  // 768 ints = 3072 bytes
                    .order(ByteOrder.nativeOrder())
    );

    /** Reusable per-thread transport for the legacy region path, with room for 8 by 8 chunks. */
    private static final ThreadLocal<ByteBuffer> REGION_BUFFER_TL = ThreadLocal.withInitial(() ->
            ByteBuffer.allocateDirect(MAX_REGION_SIZE * MAX_REGION_SIZE * PACKED_CHUNK_INTS * 4)
                    .order(ByteOrder.nativeOrder())
    );

    /** Tracks whether the DirectBuffer native methods are available (for graceful fallback). */
    private static volatile boolean directBufferAvailable = true;

    /** Reusable per-thread region transport. Starts at 16 by 16 chunks and grows when needed. */
    private static final ThreadLocal<ByteBuffer> REGION_STRIP_BUFFER_TL = ThreadLocal.withInitial(() ->
            ByteBuffer.allocateDirect(16 * 16 * PACKED_CHUNK_INTS * 4)
                    .order(ByteOrder.nativeOrder())
    );

    /*
     * Reusable quad output for each calling thread. The next bake on that thread
     * overwrites earlier results, so callers must consume or copy them first.
     */
    private static final ThreadLocal<ByteBuffer> QUAD_BAKE_BUFFER_TL = new ThreadLocal<>();

    private static ByteBuffer regionStripBuffer(int width, int height) {
        long needed = (long) width * height * PACKED_CHUNK_INTS * 4;
        ByteBuffer buf = REGION_STRIP_BUFFER_TL.get();
        if ((long) buf.capacity() < needed) {
            ByteBuffer bigger = ByteBuffer.allocateDirect((int) needed).order(ByteOrder.nativeOrder());
            REGION_STRIP_BUFFER_TL.set(bigger);
            return bigger;
        }
        return buf;
    }

    
    //  Quad baker (LoD quads for renderer bridges)
    

    /**
     * Canonical LoD quads for a region, baked entirely in C (region surface
     * generation + tiered quad baking in ONE JNI crossing).
     *
     * @param seed          world seed
     * @param dimensionId   dimension id (0 = overworld)
     * @param regionChunkX  region origin chunk X
     * @param regionChunkZ  region origin chunk Z
     * @param regionWidth   region width in chunks
     * @param regionHeight  region height in chunks
     * @param samplingStep  cubiomes sampling step (1, 2, 4, 8, 16, 32, 64, 128)
     * @param centerBlockX  player position in world blocks (tier anchor)
     * @param centerBlockZ  player position in world blocks (tier anchor)
     * @param tierBlocks    per-tier cell size in blocks (power of 2)
     * @param tierMaxDists  per-tier max Chebyshev distance in blocks (ascending)
     * @return a {@link QuadBakeResult} (direct-buffer quad records + count),
     *         or null if native is unavailable / the call fails
     */
    public static QuadBakeResult bakeRegionQuads(long seed, int dimensionId,
                                                 int regionChunkX, int regionChunkZ,
                                                 int regionWidth, int regionHeight,
                                                 int samplingStep,
                                                 int centerBlockX, int centerBlockZ,
                                                 int[] tierBlocks, int[] tierMaxDists) {
        if (!nativeAvailable || tierBlocks == null || tierMaxDists == null) return null;
        if (tierBlocks.length != tierMaxDists.length || tierBlocks.length == 0) return null;
        try {
            int capacity = nativeQuadBakeCapacity(regionChunkX, regionChunkZ,
                    regionWidth, regionHeight, centerBlockX, centerBlockZ,
                    tierBlocks, tierMaxDists);
            if (capacity <= 0) return null;
            ByteBuffer buf = quadBakeBuffer(capacity * 112L);
            int count = nativeBakeRegionQuads(seed, dimensionId,
                    regionChunkX, regionChunkZ, regionWidth, regionHeight,
                    samplingStep, centerBlockX, centerBlockZ,
                    tierBlocks, tierMaxDists, buf);
            if (count < 0) return null;
            buf.limit(count * 112);
            return new QuadBakeResult(buf, count, capacity);
        } catch (Throwable t) {
            LOGGER.warn("[NullFrameCore] bakeRegionQuads failed: {}", t.toString());
            return null;
        }
    }

    /**
     * Quad records and their count, with 112 bytes per record in native byte order.
     * The buffer starts at position zero and is limited to the used bytes. It borrows
     * per-thread storage and is overwritten by the next bake on that thread.
     */
    public record QuadBakeResult(ByteBuffer quads, int count, int capacity) {}

    private static ByteBuffer quadBakeBuffer(long bytes) {
        int needed = Math.toIntExact(Math.min(bytes, Integer.MAX_VALUE - 8));
        ByteBuffer buf = QUAD_BAKE_BUFFER_TL.get();
        if (buf == null || buf.capacity() < needed) {
            buf = ByteBuffer.allocateDirect(needed).order(ByteOrder.nativeOrder());
            QUAD_BAKE_BUFFER_TL.set(buf);
        }
        buf.clear();
        return buf;
    }

    
    //  Public API
    

    public int getTerrainHeight(long seed, int dimensionId, int blockX, int blockZ) {
        if (dimensionId == DIMENSION_NETHER) return 127;
        if (dimensionId == DIMENSION_END) {
            if (!nativeAvailable) return 63;
            return nativeGetTerrainHeight(seed, dimensionId, blockX, blockZ);
        }
        if (dimensionId != DIMENSION_OVERWORLD || !nativeAvailable) return 64;
        return nativeGetTerrainHeight(seed, dimensionId, blockX, blockZ);
    }

    public SurfaceChunkSnapshot getSurfaceChunkSnapshot(long seed,
                                                        int dimensionId,
                                                        int chunkX,
                                                        int chunkZ) {
        if (dimensionId == DIMENSION_NETHER) {
            return new SurfaceChunkSnapshot(
                    dimensionId, chunkX, chunkZ,
                    flatHeightmap(127),
                    flatSurfaceTypes(SurfaceColumnType.STONE),
                    flatBiomeIds(DEFAULT_PLAINS_BIOME_ID)
            );
        }
        if (dimensionId == DIMENSION_END && !nativeAvailable) {
            return new SurfaceChunkSnapshot(
                    dimensionId, chunkX, chunkZ,
                    flatHeightmap(63),
                    flatSurfaceTypes(SurfaceColumnType.STONE),
                    flatBiomeIds(DEFAULT_PLAINS_BIOME_ID)
            );
        }
        if (dimensionId != DIMENSION_OVERWORLD && dimensionId != DIMENSION_END) {
            return new SurfaceChunkSnapshot(
                    dimensionId, chunkX, chunkZ,
                    flatHeightmap(64),
                    flatSurfaceTypes(SurfaceColumnType.GRASS),
                    flatBiomeIds(DEFAULT_PLAINS_BIOME_ID)
            );
        }
        if (!nativeAvailable) {
            return new SurfaceChunkSnapshot(
                    dimensionId, chunkX, chunkZ,
                    flatHeightmap(64),
                    flatSurfaceTypes(SurfaceColumnType.GRASS),
                    flatBiomeIds(DEFAULT_PLAINS_BIOME_ID)
            );
        }

        int[] packed = nativeGetSurfaceChunkData(seed, dimensionId, chunkX, chunkZ);
        if (packed == null || packed.length != PACKED_CHUNK_INTS) {
            LOGGER.error("[NullFrameCore] nativeGetSurfaceChunkData returned invalid data for chunk=[{},{}]: null={}, length={}, expected={}",
                    chunkX, chunkZ, packed == null, packed == null ? -1 : packed.length, PACKED_CHUNK_INTS);
            return new SurfaceChunkSnapshot(
                    dimensionId, chunkX, chunkZ,
                    flatHeightmap(64),
                    flatSurfaceTypes(SurfaceColumnType.GRASS),
                    flatBiomeIds(DEFAULT_PLAINS_BIOME_ID)
            );
        }

        int[] heights = new int[CHUNK_COLUMN_COUNT];
        byte[] surfaceTypes = new byte[CHUNK_COLUMN_COUNT];
        int[] biomeIds = new int[CHUNK_COLUMN_COUNT];
        System.arraycopy(packed, 0, heights, 0, heights.length);
        for (int i = 0; i < surfaceTypes.length; i++) {
            surfaceTypes[i] = (byte) packed[heights.length + i];
            biomeIds[i] = packed[(CHUNK_COLUMN_COUNT * 2) + i];
        }

        return new SurfaceChunkSnapshot(dimensionId, chunkX, chunkZ, heights, surfaceTypes, biomeIds);
    }

    /**
     * Generates one Surface snapshot through a reusable direct buffer.
     * C writes the packed values into the buffer; Java copies them into snapshot arrays.
     * Falls back to {@link #getSurfaceChunkSnapshot} if the direct-buffer method is missing.
     */
    public SurfaceChunkSnapshot getSurfaceChunkSnapshotDirect(long seed,
                                                              int dimensionId,
                                                              int chunkX,
                                                              int chunkZ,
                                                              int samplingStep) {
        // Fallback for non-overworld dimensions or if native unavailable
        if (dimensionId == DIMENSION_NETHER) {
            return new SurfaceChunkSnapshot(dimensionId, chunkX, chunkZ,
                    flatHeightmap(127), flatSurfaceTypes(SurfaceColumnType.STONE),
                    flatBiomeIds(DEFAULT_PLAINS_BIOME_ID));
        }
        if (!nativeAvailable) {
            return getSurfaceChunkSnapshot(seed, dimensionId, chunkX, chunkZ);
        }

        // Try DirectBuffer path
        if (directBufferAvailable) {
            try {
                ByteBuffer buf = CHUNK_BUFFER_TL.get();
                buf.clear();
                long tRegion = System.nanoTime();
                nativeGetSurfaceChunkDataDirect(seed, dimensionId, chunkX, chunkZ, samplingStep, buf);
                StepTimings.record(StepTimings.Step.REGION_C, tRegion);

                // Read [heights][surface types][biomes] together, then create the snapshot arrays.


                long tUnpack = System.nanoTime();
                int[] packed = new int[PACKED_CHUNK_INTS];
                buf.position(0);
                buf.asIntBuffer().get(packed);
                int[] heights = Arrays.copyOfRange(packed, 0, CHUNK_COLUMN_COUNT);
                byte[] surfaceTypes = new byte[CHUNK_COLUMN_COUNT];
                for (int i = 0; i < CHUNK_COLUMN_COUNT; i++) {
                    surfaceTypes[i] = (byte) packed[CHUNK_COLUMN_COUNT + i];
                }
                int[] biomeIds = Arrays.copyOfRange(packed, CHUNK_COLUMN_COUNT * 2, PACKED_CHUNK_INTS);
                StepTimings.record(StepTimings.Step.UNPACK, tUnpack);

                return new SurfaceChunkSnapshot(dimensionId, chunkX, chunkZ, heights, surfaceTypes, biomeIds);
            } catch (UnsatisfiedLinkError e) {
                LOGGER.warn("[NullFrameCore] DirectBuffer native methods unavailable — falling back to copy-based path. Recompile native library to enable.");
                directBufferAvailable = false;
                return getSurfaceChunkSnapshot(seed, dimensionId, chunkX, chunkZ);
            }
        }

        return getSurfaceChunkSnapshot(seed, dimensionId, chunkX, chunkZ);
    }

    /**
     * Generates a region at the requested sampling step in one native call.
     * Falls back to individual requests if the native region method is missing.
     */
    public SurfaceChunkSnapshot[] getSurfaceRegionSnapshots(long seed,
                                                            int dimensionId,
                                                            int regionChunkX,
                                                            int regionChunkZ,
                                                            int regionWidth,
                                                            int regionHeight,
                                                            int samplingStep) {
        return getSurfaceRegionSnapshots(seed, dimensionId, regionChunkX, regionChunkZ,
                regionWidth, regionHeight, samplingStep, null);
    }

    /**
     * Masked region generation: only chunks where {@code mask[idx] != 0} are
     * computed by C (row-major, width × height). Null mask = compute all.
     * Skipped slots are {@code null} in the returned array — callers only read
     * slots they masked on, so this is safe and skips already-cached chunks.
     */
    public SurfaceChunkSnapshot[] getSurfaceRegionSnapshots(long seed,
                                                            int dimensionId,
                                                            int regionChunkX,
                                                            int regionChunkZ,
                                                            int regionWidth,
                                                            int regionHeight,
                                                            int samplingStep,
                                                            byte[] mask) {
        if (regionWidth <= 0 || regionHeight <= 0) {
            return new SurfaceChunkSnapshot[0];
        }
        if (!nativeAvailable || !directBufferAvailable) {
            return getRegionSurfaceSnapshotsStepFallback(seed, dimensionId,
                    regionChunkX, regionChunkZ, regionWidth, regionHeight, samplingStep, mask);
        }
        try {
            ByteBuffer buf = regionStripBuffer(regionWidth, regionHeight);
            buf.clear();
            long tRegion = System.nanoTime();
            nativeGetSurfaceRegionDataDirectMasked(seed, dimensionId,
                    regionChunkX, regionChunkZ, regionWidth, regionHeight, samplingStep, mask, buf);
            StepTimings.record(StepTimings.Step.REGION_C, tRegion);

            int chunkCount = regionWidth * regionHeight;
            SurfaceChunkSnapshot[] snapshots = new SurfaceChunkSnapshot[chunkCount];
            long tUnpack = System.nanoTime();
            // Reuse one unpacking array instead of copying the entire region into a temporary array.


            int[] packed = new int[PACKED_CHUNK_INTS];
            buf.position(0);
            for (int chunkIndex = 0; chunkIndex < chunkCount; chunkIndex++) {
                if (mask != null && mask[chunkIndex] == 0) continue;  // not computed — null
                int base = chunkIndex * PACKED_CHUNK_INTS;
                buf.position(base * 4);
                buf.asIntBuffer().get(packed);
                int[] heights = Arrays.copyOfRange(packed, 0, CHUNK_COLUMN_COUNT);
                byte[] surfaceTypes = new byte[CHUNK_COLUMN_COUNT];
                for (int i = 0; i < CHUNK_COLUMN_COUNT; i++) {
                    surfaceTypes[i] = (byte) packed[CHUNK_COLUMN_COUNT + i];
                }
                int[] biomeIds = Arrays.copyOfRange(packed, CHUNK_COLUMN_COUNT * 2, PACKED_CHUNK_INTS);
                int localChunkX = chunkIndex % regionWidth;
                int localChunkZ = chunkIndex / regionWidth;
                snapshots[chunkIndex] = new SurfaceChunkSnapshot(
                        dimensionId,
                        regionChunkX + localChunkX,
                        regionChunkZ + localChunkZ,
                        heights,
                        surfaceTypes,
                        biomeIds
                );
            }
            StepTimings.record(StepTimings.Step.UNPACK, tUnpack);
            return snapshots;
        } catch (UnsatisfiedLinkError e) {
            LOGGER.warn("[NullFrameCore] nativeGetSurfaceRegionDataDirectMasked unavailable — falling back to per-chunk region gen.");
            directBufferAvailable = false;
            return getRegionSurfaceSnapshotsStepFallback(seed, dimensionId,
                    regionChunkX, regionChunkZ, regionWidth, regionHeight, samplingStep, mask);
        }
    }

    /** Fallback that requests individual chunks using the same sampling step and compute mask. */
    private SurfaceChunkSnapshot[] getRegionSurfaceSnapshotsStepFallback(long seed,
                                                                         int dimensionId,
                                                                         int regionChunkX,
                                                                         int regionChunkZ,
                                                                         int regionWidth,
                                                                         int regionHeight,
                                                                         int samplingStep,
                                                                         byte[] mask) {
        SurfaceChunkSnapshot[] snapshots = new SurfaceChunkSnapshot[regionWidth * regionHeight];
        int index = 0;
        for (int localChunkZ = 0; localChunkZ < regionHeight; localChunkZ++) {
            for (int localChunkX = 0; localChunkX < regionWidth; localChunkX++) {
                if (mask != null && mask[index] == 0) {
                    index++;
                    continue;
                }
                snapshots[index++] = getSurfaceChunkSnapshotDirect(
                        seed, dimensionId,
                        regionChunkX + localChunkX,
                        regionChunkZ + localChunkZ,
                        samplingStep);
            }
        }
        return snapshots;
    }

    /**
     * Generates up to 8 by 8 chunks through the legacy direct-buffer region method.
     * C writes into a reusable buffer, then Java creates owned arrays for each snapshot.
     */
    public SurfaceChunkSnapshot[] getRegionSurfaceSnapshotsDirect(long seed,
                                                                  int dimensionId,
                                                                  int regionChunkX,
                                                                  int regionChunkZ,
                                                                  int regionWidth,
                                                                  int regionHeight) {
        int safeWidth = Math.max(0, Math.min(regionWidth, MAX_REGION_SIZE));
        int safeHeight = Math.max(0, Math.min(regionHeight, MAX_REGION_SIZE));
        if (safeWidth == 0 || safeHeight == 0) {
            return new SurfaceChunkSnapshot[0];
        }

        if (!nativeAvailable || !directBufferAvailable) {
            return getRegionSurfaceSnapshots(seed, dimensionId, regionChunkX, regionChunkZ, safeWidth, safeHeight);
        }

        try {
            ByteBuffer buf = REGION_BUFFER_TL.get();
            buf.clear();

            nativeGetRegionSurfaceDataDirect(seed, dimensionId,
                    regionChunkX, regionChunkZ, safeWidth, safeHeight, buf);

            int chunkCount = safeWidth * safeHeight;
            SurfaceChunkSnapshot[] snapshots = new SurfaceChunkSnapshot[chunkCount];

            // Unpack one chunk at a time from the region buffer.
            int[] packed = new int[PACKED_CHUNK_INTS];
            buf.position(0);
            for (int chunkIndex = 0; chunkIndex < chunkCount; chunkIndex++) {
                int base = chunkIndex * PACKED_CHUNK_INTS;
                buf.position(base * 4);
                buf.asIntBuffer().get(packed);
                int[] heights = Arrays.copyOfRange(packed, 0, CHUNK_COLUMN_COUNT);
                byte[] surfaceTypes = new byte[CHUNK_COLUMN_COUNT];
                for (int i = 0; i < CHUNK_COLUMN_COUNT; i++) {
                    surfaceTypes[i] = (byte) packed[CHUNK_COLUMN_COUNT + i];
                }
                int[] biomeIds = Arrays.copyOfRange(packed, CHUNK_COLUMN_COUNT * 2, PACKED_CHUNK_INTS);

                int localChunkX = chunkIndex % safeWidth;
                int localChunkZ = chunkIndex / safeWidth;
                snapshots[chunkIndex] = new SurfaceChunkSnapshot(
                        dimensionId,
                        regionChunkX + localChunkX,
                        regionChunkZ + localChunkZ,
                        heights,
                        surfaceTypes,
                        biomeIds
                );
            }

            return snapshots;
        } catch (UnsatisfiedLinkError e) {
            LOGGER.warn("[NullFrameCore] DirectBuffer region native methods unavailable — falling back to copy-based path.");
            directBufferAvailable = false;
            return getRegionSurfaceSnapshots(seed, dimensionId, regionChunkX, regionChunkZ, safeWidth, safeHeight);
        }
    }

    public SurfaceChunkSnapshot[] getRegionSurfaceSnapshots(long seed,
                                                            int dimensionId,
                                                            int regionChunkX,
                                                            int regionChunkZ,
                                                            int regionWidth,
                                                            int regionHeight) {
        int safeWidth = Math.max(0, Math.min(regionWidth, MAX_REGION_SIZE));
        int safeHeight = Math.max(0, Math.min(regionHeight, MAX_REGION_SIZE));
        if (safeWidth == 0 || safeHeight == 0) {
            return new SurfaceChunkSnapshot[0];
        }

        if (!nativeAvailable || !regionNativeMethodAvailable) {
            return getRegionSurfaceSnapshotsFallback(
                    seed, dimensionId, regionChunkX, regionChunkZ, safeWidth, safeHeight
            );
        }

        int[] packed;
        try {
            packed = nativeGetRegionSurfaceData(
                    seed,
                    dimensionId,
                    regionChunkX,
                    regionChunkZ,
                    safeWidth,
                    safeHeight
            );
        } catch (UnsatisfiedLinkError missingRegionSymbol) {
            regionNativeMethodAvailable = false;
            LOGGER.warn("[NullFrameCore] Native Cubiomes bridge is missing nativeGetRegionSurfaceData; " +
                    "falling back to per-chunk surface jobs until the library is rebuilt.");
            return getRegionSurfaceSnapshotsFallback(
                    seed, dimensionId, regionChunkX, regionChunkZ, safeWidth, safeHeight
            );
        }

        int chunkCount = safeWidth * safeHeight;
        if (packed == null || packed.length != chunkCount * PACKED_CHUNK_INTS) {
            return getRegionSurfaceSnapshotsFallback(
                    seed, dimensionId, regionChunkX, regionChunkZ, safeWidth, safeHeight
            );
        }

        SurfaceChunkSnapshot[] snapshots = new SurfaceChunkSnapshot[chunkCount];
        for (int chunkIndex = 0; chunkIndex < chunkCount; chunkIndex++) {
            int packedOffset = chunkIndex * PACKED_CHUNK_INTS;
            int[] heights = new int[CHUNK_COLUMN_COUNT];
            byte[] surfaceTypes = new byte[CHUNK_COLUMN_COUNT];
            int[] biomeIds = new int[CHUNK_COLUMN_COUNT];
            System.arraycopy(packed, packedOffset, heights, 0, CHUNK_COLUMN_COUNT);
            for (int i = 0; i < CHUNK_COLUMN_COUNT; i++) {
                surfaceTypes[i] = (byte) packed[packedOffset + CHUNK_COLUMN_COUNT + i];
                biomeIds[i] = packed[packedOffset + (CHUNK_COLUMN_COUNT * 2) + i];
            }

            int localChunkX = chunkIndex % safeWidth;
            int localChunkZ = chunkIndex / safeWidth;
            snapshots[chunkIndex] = new SurfaceChunkSnapshot(
                    dimensionId,
                    regionChunkX + localChunkX,
                    regionChunkZ + localChunkZ,
                    heights,
                    surfaceTypes,
                    biomeIds
            );
        }

        return snapshots;
    }

    /**
     * Returns packed vegetation data for a chunk.
     *
     * <p>Format: {@code [count, x1, y1, z1, blockType1, x2, y2, z2, blockType2, ...]}
     * where {@code x, z} are chunk-local (0-15), {@code y} is world Y, and
     * {@code blockType} is a {@code BlockType} constant.</p>
     *
     * <p>Returns {@code null} if native is unavailable or the call fails.
     * The caller should check for {@code null} and skip vegetation placement.</p>
     */
    public int[] getVegetation(long seed, int chunkX, int chunkZ,
                               int[] heights, int[] biomes, int[] surfaceTypes,
                               int bottomY, int topYExclusive) {
        if (!nativeAvailable) return null;
        if (heights == null || biomes == null || surfaceTypes == null) return null;
        if (heights.length != CHUNK_COLUMN_COUNT ||
                biomes.length != CHUNK_COLUMN_COUNT ||
                surfaceTypes.length != CHUNK_COLUMN_COUNT) return null;
        try {
            long tVeg = System.nanoTime();
            int[] result = nativeGetVegetation(seed, chunkX, chunkZ, heights, biomes, surfaceTypes,
                    bottomY, topYExclusive);
            StepTimings.record(StepTimings.Step.VEG, tVeg);
            return result;
        } catch (UnsatisfiedLinkError e) {
            return null;
        }
    }

    /**
     * Generates vegetation for several chunks in one JNI call.
     * Returns {@code [chunkCount, (vegCount, x,y,z,type repeated vegCount times), ...]},
     * with chunks in the same order as the coordinate arrays. Returns null if unavailable.
     */
    public int[] getRegionVegetation(long seed,
                                     int[] chunkXs, int[] chunkZs,
                                     int[] flatHeights, int[] flatBiomes, int[] flatSurfaceTypes,
                                     int bottomY, int topYExclusive) {
        if (!nativeAvailable || chunkXs == null || chunkXs.length == 0) return null;
        if (flatHeights == null || flatBiomes == null || flatSurfaceTypes == null) return null;
        try {
            long tVeg = System.nanoTime();
            int[] result = nativeGetRegionVegetation(seed, chunkXs, chunkZs,
                    flatHeights, flatBiomes, flatSurfaceTypes, bottomY, topYExclusive);
            StepTimings.record(StepTimings.Step.VEG, tVeg);
            return result;
        } catch (UnsatisfiedLinkError e) {
            return null;
        }
    }

    public static boolean isNativeAvailable() { return nativeAvailable; }

    private static native void nativeSetCorrectionAmp(int amp);

    /** Sets the shared detail-correction strength for sampling steps 16 and above; zero disables it. */
    public static void setCorrectionAmp(int amp) {
        if (nativeAvailable) nativeSetCorrectionAmp(amp);
    }

    private static native void nativeSetBiomeStep(int step);

    /** Sets the shared biome sample spacing; zero follows terrain sampling. */
    public static void setBiomeStep(int step) {
        if (nativeAvailable) nativeSetBiomeStep(step);
    }

    private static native String nativeGetBiomeName(int biomeId);

    /**
     * Returns the current-version resource name (e.g. {@code "savanna"}) for a
     * cubiomes biome id, or {@code null} if the id has no known name.
     * The game side uses this to resolve cubiomes ids to vanilla biome registry
     * entries when writing biomes into chunks.
     */
    public static String getBiomeName(int biomeId) {
        if (!nativeAvailable) return null;
        try {
            return nativeGetBiomeName(biomeId);
        } catch (UnsatisfiedLinkError e) {
            return null;
        }
    }

    
    //  Internal helpers
    

    private static int[] flatHeightmap(int y) {
        int[] out = new int[256];
        Arrays.fill(out, y);
        return out;
    }

    private static byte[] flatSurfaceTypes(byte type) {
        byte[] out = new byte[256];
        Arrays.fill(out, type);
        return out;
    }

    private static int[] flatBiomeIds(int biomeId) {
        int[] out = new int[256];
        Arrays.fill(out, biomeId);
        return out;
    }

    private SurfaceChunkSnapshot[] getRegionSurfaceSnapshotsFallback(long seed,
                                                                     int dimensionId,
                                                                     int regionChunkX,
                                                                     int regionChunkZ,
                                                                     int regionWidth,
                                                                     int regionHeight) {
        SurfaceChunkSnapshot[] snapshots = new SurfaceChunkSnapshot[regionWidth * regionHeight];
        int index = 0;
        for (int localChunkZ = 0; localChunkZ < regionHeight; localChunkZ++) {
            for (int localChunkX = 0; localChunkX < regionWidth; localChunkX++) {
                snapshots[index++] = getSurfaceChunkSnapshot(
                        seed,
                        dimensionId,
                        regionChunkX + localChunkX,
                        regionChunkZ + localChunkZ
                );
            }
        }
        return snapshots;
    }

    private static String nativeResourcePathForCurrentOs() {
        String os = System.getProperty("os.name", "").toLowerCase(Locale.ROOT);
        if (os.contains("win"))   return "/native/cubiomes_bridge.dll";
        if (os.contains("linux")) return "/native/libcubiomes_bridge.so";
        if (os.contains("mac"))   return "/native/libcubiomes_bridge.dylib";
        return null;
    }

    /**
     * Returns a path to the native library that the JVM can load.
     *
     * The library is copied out of the jar to a file whose name contains a hash of its contents,
     * in a per-user folder under the system temp directory. If that file already exists with
     * identical bytes it is reused, so there is one copy per library version instead of one per
     * launch. Windows cannot delete a DLL that is still loaded, so the previous approach (a new
     * temp file per launch plus deleteOnExit) left a copy behind every time. If the shared
     * location cannot be used, this falls back to a unique temp file.
     */
    private static Path extractNativeLibrary(String resourcePath) throws IOException {
        String suffix = resourcePath.endsWith(".dll")    ? ".dll"
                : resourcePath.endsWith(".dylib")  ? ".dylib"
                  : ".so";
        byte[] library;
        try (InputStream stream = Cubiomes.class.getResourceAsStream(resourcePath)) {
            if (stream == null) {
                throw new IOException("Native library missing from jar: " + resourcePath
                        + " — compile the C library with CMake and copy the output to "
                        + "src/main/resources/native/");
            }
            library = stream.readAllBytes();
        }
        try {
            Path directory = privateNativeDirectory();
            Path target = directory.resolve("cubiomes_bridge-" + contentHash(library) + suffix);
            if (!hasContent(target, library)) {
                writeNativeLibrary(directory, target, library, suffix);
            }
            // The bytes are verified on every start, so a replaced or damaged file is never loaded.
            if (hasContent(target, library)) {
                removeLegacyTempFiles();
                return target;
            }
        } catch (IOException | RuntimeException e) {
            LOGGER.warn("[NullFrameCore] Could not use the shared native library location, "
                    + "using a temporary file instead.", e);
        }
        return extractToUniqueTempFile(library, suffix);
    }

    /** The old behaviour, kept as a fallback: one new temp file for this run. */
    private static Path extractToUniqueTempFile(byte[] library, String suffix) throws IOException {
        Path tempFile = Files.createTempFile("nullframe-cubiomes-", suffix);
        tempFile.toFile().deleteOnExit();
        Files.write(tempFile, library);
        return tempFile;
    }

    /** A folder under the temp directory that only the current user can change. */
    private static Path privateNativeDirectory() throws IOException {
        String user = System.getProperty("user.name", "user").replaceAll("[^A-Za-z0-9._-]", "_");
        Path directory = Path.of(System.getProperty("java.io.tmpdir")).resolve("nullframe-core-" + user);
        boolean posix = FileSystems.getDefault().supportedFileAttributeViews().contains("posix");
        if (!posix) {
            // Windows: the temp directory already belongs to the current user.
            Files.createDirectories(directory);
            return directory;
        }
        Files.createDirectories(directory, PosixFilePermissions.asFileAttribute(
                PosixFilePermissions.fromString("rwx------")));
        if (Files.isSymbolicLink(directory) || !Files.isDirectory(directory)) {
            throw new IOException("Native library folder is not a plain directory: " + directory);
        }
        UserPrincipal self = FileSystems.getDefault().getUserPrincipalLookupService()
                .lookupPrincipalByName(System.getProperty("user.name"));
        Set<PosixFilePermission> permissions = Files.getPosixFilePermissions(directory);
        if (!Files.getOwner(directory).equals(self)
                || permissions.contains(PosixFilePermission.GROUP_WRITE)
                || permissions.contains(PosixFilePermission.OTHERS_WRITE)) {
            throw new IOException("Native library folder is not private: " + directory);
        }
        return directory;
    }

    /** Writes to a partial file first and renames it, so another instance never sees half a file. */
    private static void writeNativeLibrary(Path directory, Path target, byte[] library, String suffix)
            throws IOException {
        Path partial = Files.createTempFile(directory, "partial-", suffix);
        try {
            Files.write(partial, library);
            try {
                Files.move(partial, target, StandardCopyOption.ATOMIC_MOVE);
            } catch (AtomicMoveNotSupportedException e) {
                Files.move(partial, target, StandardCopyOption.REPLACE_EXISTING);
            }
        } catch (IOException e) {
            // Another instance may have created the same file at the same moment.
            if (!hasContent(target, library)) throw e;
        } finally {
            try {
                Files.deleteIfExists(partial);
            } catch (IOException ignored) {
                // Leave it; it is only a few hundred kilobytes in the temp folder.
            }
        }
    }

    private static boolean hasContent(Path file, byte[] expected) {
        try {
            return Files.isRegularFile(file, LinkOption.NOFOLLOW_LINKS)
                    && Files.size(file) == expected.length
                    && Arrays.equals(Files.readAllBytes(file), expected);
        } catch (IOException e) {
            return false;
        }
    }

    private static String contentHash(byte[] data) {
        try {
            byte[] digest = MessageDigest.getInstance("SHA-256").digest(data);
            return HexFormat.of().formatHex(digest, 0, 8);
        } catch (NoSuchAlgorithmException e) {
            throw new IllegalStateException(e);
        }
    }

    /**
     * Removes copies left behind by earlier versions, which created a new file on every launch.
     * Best effort: a file that is still in use by a running game cannot be deleted and is skipped.
     */
    private static void removeLegacyTempFiles() {
        Path temp = Path.of(System.getProperty("java.io.tmpdir"));
        int removed = 0;
        try (DirectoryStream<Path> files = Files.newDirectoryStream(temp, "nullframe-cubiomes-*")) {
            for (Path file : files) {
                try {
                    if (Files.isRegularFile(file, LinkOption.NOFOLLOW_LINKS) && Files.deleteIfExists(file)) {
                        removed++;
                    }
                } catch (IOException ignored) {
                    // In use by another running game; leave it.
                }
            }
        } catch (IOException | RuntimeException ignored) {
            // Cleaning up is optional.
        }
        if (removed > 0) {
            LOGGER.info("[NullFrameCore] Removed {} leftover native library copies from the temp folder.", removed);
        }
    }
}
