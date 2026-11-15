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

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.List;

/** Owned, immutable paletted sections. No references to reusable native/TLS memory escape JNI. */
public final class NativeChunkData {
    public static final int FORMAT = 0x4e464331;
    public static final int AIR = 0, STONE = 64, DIRT = 65, GRASS = 66, SNOWY_GRASS = 67,
            SAND = 68, SANDSTONE = 69, WATER = 70, BEDROCK = 71, DEEPSLATE = 72,
            MYCELIUM = 73, PODZOL = 74, TERRACOTTA = 75, WHITE_TERRACOTTA = 76,
            LIGHT_GRAY_TERRACOTTA = 77, YELLOW_TERRACOTTA = 78, ORANGE_TERRACOTTA = 79,
            RED_TERRACOTTA = 80, GRAVEL = 81, COARSE_DIRT = 82,
            BROWN_CAP_BASE = 96, RED_CAP_BASE = 128, MAX_BLOCK_ID = 159;

    public static final class Section {
        private final int index, bits;
        private final int[] palette;
        private final long[] storage;
        private Section(int index, int bits, int[] palette, long[] storage) {
            this.index = index; this.bits = bits; this.palette = palette; this.storage = storage;
        }
        public int index() { return index; }
        public int bits() { return bits; }
        public int[] paletteIds() { return palette.clone(); }
        /** Minecraft containers are mutable, so every installation receives its own storage. */
        public long[] copyStorage() { return storage.clone(); }
        public int blockId(int index) {
            if (index < 0 || index >= 4096) throw new IndexOutOfBoundsException(index);
            if (bits == 0) return palette[0];
            int perWord = 64 / bits;
            return palette[(int) (storage[index / perWord] >>> ((index % perWord) * bits)) & ((1 << bits) - 1)];
        }
        public long estimatedBytes() { return 48L + 4L * palette.length + 8L * storage.length; }
    }

    private final NativeChunkOptions options;
    private final List<Section> sections;
    private final int[] diagnosticPositions;
    private final long estimatedBytes;

    private NativeChunkData(NativeChunkOptions options, List<Section> sections, int[] diagnosticPositions) {
        this.options = options; this.sections = List.copyOf(sections); this.diagnosticPositions = diagnosticPositions;
        this.estimatedBytes = 64L + 4L * diagnosticPositions.length
                + sections.stream().mapToLong(Section::estimatedBytes).sum();
    }
    public NativeChunkOptions options() { return options; }
    public List<Section> sections() { return sections; }
    public int[] diagnosticPositions() { return diagnosticPositions.clone(); }
    public long estimatedBytes() { return estimatedBytes; }

    static SurfaceChunkSnapshot decode(int dimension, int x, int z, NativeChunkOptions expected, byte[] encoded) {
        if (encoded == null || encoded.length < 3096) throw new IllegalStateException("Missing native chunk result");
        return decode(dimension, x, z, expected, ByteBuffer.wrap(encoded));
    }

    /** The reusable transport buffer is borrowed only for this call; all retained arrays are owned. */
    static SurfaceChunkSnapshot decode(int dimension, int x, int z, NativeChunkOptions expected, ByteBuffer encoded) {
        if (encoded == null || encoded.remaining() < 3096) throw new IllegalStateException("Missing native chunk result");
        ByteBuffer in = encoded.slice().order(ByteOrder.nativeOrder());
        if (in.getInt() != FORMAT) throw new IllegalStateException("Native chunk format mismatch; update NullFrame native library");
        int bottom = in.getInt(), top = in.getInt(), flags = in.getInt();
        NativeChunkOptions options = new NativeChunkOptions(bottom, top, (flags & 1) != 0, (flags & 2) != 0,
                (flags >>> 2) - 1);
        if (!options.equals(expected)) throw new IllegalStateException("Native chunk options mismatch");
        int[] heights = new int[256], biomes = new int[256];
        byte[] types = new byte[256];
        in.asIntBuffer().get(heights); in.position(in.position() + 1024);
        for (int i = 0; i < 256; i++) types[i] = (byte) in.getInt();
        in.asIntBuffer().get(biomes); in.position(in.position() + 1024);
        int count = in.getInt(), sectionLimit = (options.topYExclusive() - options.bottomY()) / 16;
        if (count < 0 || count > sectionLimit) throw new IllegalStateException("Invalid section count");
        java.util.ArrayList<Section> sections = new java.util.ArrayList<>(count);
        int previous = -1;
        for (int i = 0; i < count; i++) {
            int index = in.getInt(), bits = in.getInt(), paletteSize = in.getInt();
            if (index <= previous || index >= sectionLimit || (bits != 0 && (bits < 4 || bits > 8))
                    || paletteSize < 1 || paletteSize > 256 || (bits == 0 && paletteSize != 1)
                    || (bits != 0 && paletteSize > (1 << bits))) throw new IllegalStateException("Invalid native palette");
            previous = index;
            int[] palette = new int[paletteSize];
            for (int j = 0; j < paletteSize; j++) {
                palette[j] = in.getInt();
                if (palette[j] < 0 || palette[j] > MAX_BLOCK_ID) throw new IllegalStateException("Invalid native block ID");
            }
            int words = in.getInt(), required = bits == 0 ? 0 : (4096 + 64 / bits - 1) / (64 / bits);
            if (words != required) throw new IllegalStateException("Invalid native section storage");
            long[] storage = new long[words];
            in.asLongBuffer().get(storage); in.position(in.position() + words * 8);
            sections.add(new Section(index, bits, palette, storage));
        }
        int diagnostics = in.getInt();
        if (diagnostics < 0 || diagnostics > 512 || in.remaining() != diagnostics * 4)
            throw new IllegalStateException("Invalid diagnostic positions");
        int[] positions = new int[diagnostics];
        for (int i = 0; i < diagnostics; i++) {
            positions[i] = in.getInt();
            if (positions[i] < 0 || positions[i] >= sectionLimit * 4096)
                throw new IllegalStateException("Invalid diagnostic position");
        }
        return new SurfaceChunkSnapshot(dimension, x, z, heights, types, biomes,
                new NativeChunkData(options, sections, positions));
    }
}
