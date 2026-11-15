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

import java.util.Locale;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicLongArray;

/**
 * Shared elapsed-time counters for Core and Myelin.
 *
 * <p>Recording adds a nanoTime duration and an operation count to atomic counters.
 * Formatting a report allocates strings. Durations from concurrent work can add up
 * to more than the elapsed reporting window; these are not CPU-time measurements.</p>
 */
public final class StepTimings {

    public enum Step {
        // Server scheduling counters.
        SCHEDULE,          // Surface scheduling and native job submission.
        FILL_GAPS,         // Reserved gap-fill counter.
        DRAIN,             // Reserved completion-drain counter.
        // Native generation and result decoding.
        REGION_C,          // native cubiomes region JNI call (noise + interp + correction + classify)
        UNPACK,            // Java unpack of the region buffer into SurfaceChunkSnapshots
        VEG,               // native vegetation JNI call
        // server, preview-sender thread
        PACKET_BUILD,      // building SurfaceChunkBatchS2CPacket entries
        SEND,              // packet encode + netty write (ServerPlayNetworking.send)
        // Chunk construction and installation.
        PASTE_SURFACE,     // Reserved surface-placement counter.
        PASTE_BEDROCK,     // Reserved bottom-fill counter.
        PASTE_VEG,         // Reserved vegetation-placement counter.
        PASTE_LIGHT,       // lighting checkBlock pass
        NATIVE_CHUNK,      // terrain + vegetation + section packing; streamed chunks exclude Java callback time
        INSTALL_SECTIONS,  // Java palette translation, owned storage installation, and section counts
        // client
        CLIENT_DECODE,     // packet codec decode (netty thread)
        CLIENT_STORE,      // storeSurfacePreview (client main thread)
        CLIENT_MESH        // preview mesh build (client render thread)
    }

    private static final AtomicLongArray NANOS = new AtomicLongArray(Step.values().length);
    private static final AtomicLongArray COUNTS = new AtomicLongArray(Step.values().length);
    private static final AtomicLong SINCE_MILLIS = new AtomicLong(System.currentTimeMillis());

    private StepTimings() {
    }

    /** Zeroes every accumulator and restarts the elapsed-window clock. */
    public static void reset() {
        for (Step s : Step.values()) {
            NANOS.set(s.ordinal(), 0L);
            COUNTS.set(s.ordinal(), 0L);
        }
        SINCE_MILLIS.set(System.currentTimeMillis());
    }

    /** Copies durations followed by operation counts, for comparison with a later snapshot. */
    public static long[] snapshot() {
        int n = Step.values().length;
        long[] out = new long[n * 2];
        for (int i = 0; i < n; i++) {
            out[i] = NANOS.get(i);
            out[n + i] = COUNTS.get(i);
        }
        return out;
    }

    /**
     * Formats durations and operation counts added since the supplied snapshot.
     * Durations from parallel work may overlap, so their sum can exceed wall time.
     */
    public static String delta(long[] before) {
        int n = Step.values().length;
        if (before == null || before.length < n * 2) return "(no snapshot)";
        StringBuilder sb = new StringBuilder();
        for (Step s : Step.values()) {
            int i = s.ordinal();
            long dn = NANOS.get(i) - before[i];
            long dc = COUNTS.get(i) - before[n + i];
            if (dc == 0) continue;
            double ms = dn / 1_000_000.0;
            if (sb.length() > 0) sb.append(' ');
            sb.append(String.format(Locale.ROOT, "%s=%.0fms(%d)", s.name(), ms, dc));
        }
        return sb.length() > 0 ? sb.toString() : "none";
    }

    /** Records the wall time between {@code startNanos} and now for the given step. */
    public static void record(Step step, long startNanos) {
        long elapsed = System.nanoTime() - startNanos;
        if (elapsed < 0) elapsed = 0;
        NANOS.addAndGet(step.ordinal(), elapsed);
        COUNTS.incrementAndGet(step.ordinal());
    }

    /** Total number of recorded ops across the given steps (for "has anything moved?" checks). */
    public static long countsSum(Step... steps) {
        long sum = 0;
        for (Step s : steps) {
            sum += COUNTS.get(s.ordinal());
        }
        return sum;
    }

    /**
     * Formats totals, average duration and duration as a percentage of the reporting window.
     * Percentages can add up to more than 100 when recorded operations overlap.
     */
    public static String summary(String label) {
        long elapsedMs = Math.max(1, System.currentTimeMillis() - SINCE_MILLIS.get());
        StringBuilder sb = new StringBuilder();
        sb.append("[Myelin-STEPS] ").append(label).append(" (since ").append(elapsedMs).append("ms): ");
        boolean any = false;
        for (Step s : Step.values()) {
            long n = NANOS.get(s.ordinal());
            long c = COUNTS.get(s.ordinal());
            if (c == 0) continue;
            any = true;
            double ms = n / 1_000_000.0;
            double pct = 100.0 * ms / elapsedMs;
            sb.append(String.format(Locale.ROOT, "%s=%.0fms(%d,%.2fms/op,%.0f%%wall) ",
                    s.name(), ms, c, ms / c, pct));
        }
        if (!any) {
            sb.append("no activity yet — run /myelin steps reset, then move/fly");
        }
        return sb.toString();
    }
}
