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
import java.util.Objects;

/**
 * Vertical material runs borrowed from a compact-column request.
 * Y bounds are relative to bottomY, with an exclusive upper bound. Air runs are
 * included. The view is valid only inside the request callback and holds no
 * Minecraft or Distant Horizons objects.
 */
public final class CompactTerrainColumns {
    static final int FORMAT = 0x4e464c31;
    private final ByteBuffer data;
    private final int width, height, bottomY, topY;
    private boolean valid = true;

    CompactTerrainColumns(ByteBuffer buffer, int width, int height, int bottomY, int topY) {
        data = buffer.asReadOnlyBuffer().order(ByteOrder.nativeOrder());
        if (data.remaining() < 24 || data.getInt(0) != FORMAT || data.getInt(4) != width
                || data.getInt(8) != height || data.getInt(12) != bottomY || data.getInt(16) != topY
                || data.getInt(20) != width * height) throw new IllegalStateException("Invalid compact column header");
        this.width = width; this.height = height; this.bottomY = bottomY; this.topY = topY;
    }

    public int width() { checkValid(); return width; }
    public int height() { checkValid(); return height; }
    public int bottomY() { checkValid(); return bottomY; }
    public int topYExclusive() { checkValid(); return topY; }
    public int encodedBytes() { checkValid(); return data.limit(); }

    public interface Visitor {
        void beginColumn(int x, int z, int biomeId, int runCount);
        void run(int materialId, int bottomRelativeY, int topRelativeY);
        void endColumn();
    }

    /** Columns may be delivered in chunk order. Each position is delivered exactly once. */
    public void visit(Visitor visitor) {
        checkValid(); Objects.requireNonNull(visitor);
        ByteBuffer input = data.duplicate().order(ByteOrder.nativeOrder());
        input.position(24);
        int worldHeight = topY - bottomY, count = width * height;
        boolean[] seen = new boolean[count];
        for (int column = 0; column < count; column++) {
            if (input.remaining() < 12) throw new IllegalStateException("Truncated compact column");
            int index = input.getInt(), biome = input.getInt(), runs = input.getInt();
            if (index < 0 || index >= count || seen[index] || biome < 0 || biome > 255
                    || runs < 1 || runs > worldHeight || input.remaining() < runs * 8)
                throw new IllegalStateException("Invalid compact column record");
            seen[index] = true;
            visitor.beginColumn(index % width, index / width, biome, runs);
            int cursor = 0, previousId = -1;
            for (int run = 0; run < runs; run++) {
                int id = input.getInt(), bounds = input.getInt();
                int lo = bounds & 0xffff, hi = bounds >>> 16;
                if (id < 0 || id > NativeChunkData.MAX_BLOCK_ID || id == previousId
                        || lo != cursor || hi <= lo || hi > worldHeight)
                    throw new IllegalStateException("Non-contiguous or invalid compact terrain run");
                visitor.run(id, lo, hi);
                previousId = id; cursor = hi;
            }
            if (cursor != worldHeight) throw new IllegalStateException("Incomplete compact terrain column");
            visitor.endColumn();
        }
        if (input.hasRemaining()) throw new IllegalStateException("Trailing compact column bytes");
    }

    void invalidate() { valid = false; }
    private void checkValid() {
        if (!valid) throw new IllegalStateException("Compact terrain view escaped its callback");
    }
}
