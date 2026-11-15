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
typedef struct { unsigned char *data; int capacity, size; } CompactColumnWriter;
static void cc_int(CompactColumnWriter *w, int value) {
    if (w->size <= w->capacity - 4) memcpy(w->data + w->size, &value, 4);
    w->size += 4;
}
static int cc_terrain_id(int y, int terrain, int type, int biome, int bottom) {
    if (y < terrain - 3) return y == bottom ? NC_BEDROCK : y < 0 ? NC_DEEPSLATE : NC_STONE;
    if (y > terrain && y < NC_SEA_LEVEL) return NC_WATER;
    int id = nc_column(type, terrain, terrain, y, 3, biome);
    if (id) return id;
    return 0;
}
static void cc_column(CompactColumnWriter *w, int index, int biome, const uint16_t *blocks,
                      int local_index, int terrain, int type, int bottom, int top) {
    cc_int(w, index); cc_int(w, biome);
    int count_at = w->size, count = 0;
    cc_int(w, 0);
    int previous = -1, start = 0;
    for (int rel = 0; rel <= top - bottom; rel++) {
        int id = rel == top - bottom ? -1 : blocks
                ? blocks[rel * 256 + local_index] & ~NC_DIAGNOSTIC
                : cc_terrain_id(bottom + rel, terrain, type, biome, bottom);
        if (id != previous) {
            if (previous >= 0) {
                cc_int(w, previous); cc_int(w, start | (rel << 16)); count++;
            }
            previous = id; start = rel;
        }
    }
    if (count_at <= w->capacity - 4) memcpy(w->data + count_at, &count, 4);
}

JNIEXPORT jint JNICALL
Java_com_nullframe_core_cubiomes_Cubiomes_nativeWriteCompactColumnsV1(JNIEnv *env, jclass clazz,
        jlong seed, jint min_x, jint min_z, jint width, jint height, jint spacing,
        jint bottom, jint top, jboolean vegetation, jobject output) {
    (void)clazz;
    unsigned char *data = output ? (*env)->GetDirectBufferAddress(env, output) : NULL;
    jlong capacity = output ? (*env)->GetDirectBufferCapacity(env, output) : 0;
    if (!data || capacity < 24 || capacity > INT32_MAX || width < 1 || width > 64 || height < 1 || height > 64
            || spacing < 1 || spacing > 512 || (spacing & (spacing - 1)) || top <= bottom
            || (int64_t)top - bottom > 4096 || bottom < -1000000 || top > 1000000
            || min_x < INT32_MIN + 32 || min_z < INT32_MIN + 32
            || (int64_t)min_x + (width - 1) * spacing > INT32_MAX - 32
            || (int64_t)min_z + (height - 1) * spacing > INT32_MAX - 32) {
        jclass error = (*env)->FindClass(env, "java/lang/IllegalArgumentException");
        if (error) (*env)->ThrowNew(env, error, "Invalid compact terrain request");
        return 0;
    }
    Generator *g = get_cached_generator((uint64_t)seed);
    if (!g) {
        jclass error = (*env)->FindClass(env, "java/lang/OutOfMemoryError");
        if (error) (*env)->ThrowNew(env, error, "Unable to initialize native terrain generator");
        return 0;
    }
    CompactColumnWriter writer = {data, (int)capacity, 0};
    cc_int(&writer, 0x4e464c31); cc_int(&writer, width); cc_int(&writer, height);
    cc_int(&writer, bottom); cc_int(&writer, top); cc_int(&writer, width * height);
    if (!vegetation) {
        ClassifyMemo memo; classify_memo_init(&memo);
        for (int z = 0; z < height; z++) for (int x = 0; x < width; x++) {
            int64_t noise[6];
            int biome = sample_biome_noise_pair(&g->bn, (min_x + x * spacing) / 4.0,
                    (min_z + z * spacing) / 4.0, noise, 1);
            int terrain = round_to_int((float)(noise[NP_DEPTH] / 76.0));
            cc_column(&writer, z * width + x, biome, NULL, 0, terrain,
                    classify_memo_get(&memo, biome), bottom, top);
        }
    } else {
        uint16_t *blocks = malloc((size_t)(top - bottom) * 256 * sizeof(uint16_t));
        int *veg = malloc(VEG_BUFFER_INTS * sizeof(int));
        if (!blocks || !veg) {
            free(blocks); free(veg);
            jclass error = (*env)->FindClass(env, "java/lang/OutOfMemoryError");
            if (error) (*env)->ThrowNew(env, error, "Unable to allocate compact vegetation scratch");
            return 0;
        }
        /* Group sampled coordinates by their native chunk. Build each complete template once. */
        for (int gz = 0; gz < height;) {
            int cz = floor_div_world(min_z + gz * spacing, 16), end_z = gz + 1;
            while (end_z < height && floor_div_world(min_z + end_z * spacing, 16) == cz) end_z++;
            for (int gx = 0; gx < width;) {
                int cx = floor_div_world(min_x + gx * spacing, 16), end_x = gx + 1;
                while (end_x < width && floor_div_world(min_x + end_x * spacing, 16) == cx) end_x++;
                int snapshot[PACKED_CHUNK_INTS];
                myelin_get_surface_chunk_data(seed, CB_DIM_OVERWORLD, cx, cz, snapshot, 1);
                nc_build_blocks(blocks, snapshot, CB_DIM_OVERWORLD, bottom, top, 1, 1, 3, seed, cx, cz, veg);
                for (int z = gz; z < end_z; z++) for (int x = gx; x < end_x; x++) {
                    int local = (min_z + z * spacing - cz * 16) * 16 + min_x + x * spacing - cx * 16;
                    cc_column(&writer, z * width + x, snapshot[512 + local], blocks, local, 0, 0, bottom, top);
                }
                gx = end_x;
            }
            gz = end_z;
        }
        free(blocks); free(veg);
    }
    return writer.size > writer.capacity ? -writer.size : writer.size;
}
