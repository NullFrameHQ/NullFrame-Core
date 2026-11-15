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
#define NC_STONE 64
#define NC_DIRT 65
#define NC_GRASS 66
#define NC_SNOWY_GRASS 67
#define NC_SAND 68
#define NC_SANDSTONE 69
#define NC_WATER 70
#define NC_BEDROCK 71
#define NC_DEEPSLATE 72
#define NC_MYCELIUM 73
#define NC_PODZOL 74
#define NC_TERRACOTTA 75
#define NC_WHITE_TERRACOTTA 76
#define NC_LIGHT_GRAY_TERRACOTTA 77
#define NC_YELLOW_TERRACOTTA 78
#define NC_ORANGE_TERRACOTTA 79
#define NC_RED_TERRACOTTA 80
#define NC_GRAVEL 81
#define NC_COARSE_DIRT 82
#define NC_DIAGNOSTIC 0x8000
#define NC_SEA_LEVEL 63 /* Exclusive: the highest source-water block is Y=62. */

typedef struct { unsigned char *data; int size, capacity, failed, fixed; } NativeChunkWriter;
static void nc_write(NativeChunkWriter *w, const void *data, int bytes) {
    if (w->failed || bytes < 0 || w->size > 1048576 - bytes) { w->failed = 1; return; }
    int required = w->size + bytes;
    if (required > w->capacity) {
        if (w->fixed) { w->failed = 1; return; }
        int cap = w->capacity ? w->capacity : 8192;
        while (cap < required) cap *= 2;
        void *next = realloc(w->data, (size_t)cap);
        if (!next) { w->failed = 1; return; }
        w->data = (unsigned char *)next; w->capacity = cap;
    }
    memcpy(w->data + w->size, data, (size_t)bytes); w->size += bytes;
}
static void nc_int(NativeChunkWriter *w, int value) { nc_write(w, &value, 4); }
static int nc_min(int a, int b) { return a < b ? a : b; }
static int nc_max(int a, int b) { return a > b ? a : b; }

/* Biomes select ground materials; Overworld surface water is height-based. */
static int nc_snowy(int b) {
    switch (b) { case 11: case 12: case 13: case 26: case 30: case 31: case 158:
        case 140: case 178: case 179: case 181: return 1; default: return 0; }
}
static int nc_badlands(int b) {
    switch (b) { case 37: case 38: case 39: case 165: case 166: case 167: return 1; default: return 0; }
}
static int nc_sandy(int b) { return b == 2 || b == 17 || b == 130 || b == 16 || b == 15; }
static int nc_stony(int b) { return b == 25 || b == 182 || b == 180 || b == 131 || b == 162; }
static int nc_terracotta(int y) {
    int band = ((y % 16) + 16) % 16;
    return band < 5 ? NC_WHITE_TERRACOTTA + band : NC_TERRACOTTA;
}
static int nc_surface(int b, int y) {
    if (nc_badlands(b)) return nc_terracotta(y);
    if (b == 14 || b == 15) return NC_MYCELIUM;
    if (b == gravelly_mountains || b == modified_gravelly_mountains) return NC_GRAVEL;
    if (nc_sandy(b)) return NC_SAND;
    if (b == 32 || b == 33 || b == 160 || b == 161 || b == 168 || b == 169) return NC_PODZOL;
    if (nc_stony(b)) return NC_STONE;
    return nc_snowy(b) ? NC_SNOWY_GRASS : NC_GRASS;
}
static int nc_subsurface(int b, int y) {
    if (nc_badlands(b)) return nc_terracotta(y);
    if (nc_sandy(b)) return NC_SANDSTONE;
    return nc_stony(b) ? NC_STONE : NC_DIRT;
}
static int nc_column(int type, int terrain_y, int surface_y, int y, int depth, int biome) {
    if (type == SURFACE_WATER) {
        if (y > terrain_y && y <= surface_y) return NC_WATER;
        if (y == terrain_y) return NC_SAND;
        if (y == terrain_y - 1) return NC_SANDSTONE;
        if (y < terrain_y - 1) return depth <= 1 ? 0 :
                (y >= terrain_y - nc_min(2, depth) ? NC_SAND : NC_STONE);
        return 0;
    }
    if (nc_snowy(biome)) {
        if (y == terrain_y + 1) return BLK_SNOW_LAYER;
        if (y == terrain_y) return NC_SNOWY_GRASS;
        return y < terrain_y && y >= terrain_y - depth ? NC_DIRT : 0;
    }
    if (y > terrain_y || y < terrain_y - depth) return 0;
    return y < terrain_y ? nc_subsurface(biome, y) : nc_surface(biome, y);
}

static void nc_build_blocks(uint16_t *blocks, int *snapshot, int dimension, int bottom, int top,
                            int fill_bottom, int vegetation, int surface_depth, long long seed, int cx, int cz, int *veg) {
    int height = top - bottom;
    memset(blocks, 0, (size_t)height * 256 * sizeof(uint16_t));
    int depth = surface_depth;
    if (depth < 0) { /* Compatibility for the original four-argument Core options. */
        int low = snapshot[0], high = snapshot[0];
        for (int i = 1; i < 256; i++) { low = nc_min(low, snapshot[i]); high = nc_max(high, snapshot[i]); }
        depth = high - low <= 1 ? 1 : (high - low <= 3 ? 2 : 4);
    }
    int vegetation_types[256];
    for (int i = 0; i < 256; i++) {
        int y = snapshot[i], type = snapshot[256 + i], biome = snapshot[512 + i];
        int flooded = dimension == CB_DIM_OVERWORLD && y < NC_SEA_LEVEL - 1;
        int surface = flooded ? NC_SEA_LEVEL - 1 : y;
        vegetation_types[i] = flooded ? SURFACE_WATER : type;
        int begin = nc_max(bottom, y - depth);
        int end = nc_min(top - 1, y + (!flooded && type != SURFACE_WATER ? nc_snowy(biome) : 0));
        for (int by = begin; by <= end; by++) {
            int id = nc_column(type, y, y, by, depth, biome);
            if (id) {
                if (by == surface || (!flooded && type != SURFACE_WATER && nc_snowy(biome) && by == y + 1)) id |= NC_DIAGNOSTIC;
                blocks[(by - bottom) * 256 + i] = (uint16_t)id;
            }
        }
        /* Fill only above the sampled ground, preserving the hollow underside when fillBottom is OFF. */
        if (flooded) for (int by = nc_max(bottom, y + 1); by <= nc_min(top - 1, surface); by++)
            blocks[(by - bottom) * 256 + i] = (uint16_t)(NC_WATER | (by == surface ? NC_DIAGNOSTIC : 0));
        if (fill_bottom && dimension == CB_DIM_OVERWORLD) {
            int cap = nc_min(top - 1, y - depth - 1);
            for (int by = bottom; by <= cap; by++)
                blocks[(by - bottom) * 256 + i] = (uint16_t)(by <= bottom ? NC_BEDROCK : by < 0 ? NC_DEEPSLATE : NC_STONE);
        }
    }
    if (vegetation) {
        VegBuffer vb;
        vegbuf_init(&vb, veg, MAX_VEG_BLOCKS);
        vb.use_templates = 1;
        generate_chunk_vegetation((uint64_t)seed, cx, cz, snapshot, snapshot + 512, vegetation_types,
                bottom, top, vb);
        for (int i = 0; i < veg[0]; i++) {
            int *v = veg + 1 + i * 4;
            if (v[1] >= bottom && v[1] < top)
                blocks[(v[1] - bottom) * 256 + v[2] * 16 + v[0]] = (uint16_t)v[3];
        }
    }
}

static void nc_encode(NativeChunkWriter *w, uint16_t *blocks, int *snapshot, int bottom, int top,
                       int fill_bottom, int vegetation, int surface_depth) {
    nc_int(w, 0x4e464331); nc_int(w, bottom); nc_int(w, top);
    nc_int(w, (fill_bottom ? 1 : 0) | (vegetation ? 2 : 0) | ((surface_depth + 1) << 2));
    nc_write(w, snapshot, PACKED_CHUNK_INTS * 4);
    int count_at = w->size, section_count = 0;
    nc_int(w, 0);
    int diagnostics[512], diagnostic_count = 0;
    for (int section = 0; section < (top - bottom) / 16; section++) {
        int palette[256], lookup[256], palette_size = 0;
        unsigned char indices[4096];
        for (int i = 0; i < 256; i++) lookup[i] = -1;
        int non_air = 0;
        for (int i = 0; i < 4096; i++) {
            int value = blocks[section * 4096 + i];
            if ((value & NC_DIAGNOSTIC) && diagnostic_count < 512)
                diagnostics[diagnostic_count++] = section * 4096 + i;
            int id = value & ~NC_DIAGNOSTIC;
            non_air |= id;
            if (lookup[id] < 0) { lookup[id] = palette_size; palette[palette_size++] = id; }
            indices[i] = (unsigned char)lookup[id];
        }
        if (!non_air) continue;
        section_count++;
        int bits = 0;
        if (palette_size > 1) { bits = 4; while ((1 << bits) < palette_size) bits++; }
        nc_int(w, section); nc_int(w, bits); nc_int(w, palette_size);
        nc_write(w, palette, palette_size * 4);
        int per_word = bits ? 64 / bits : 0;
        int words = bits ? (4096 + per_word - 1) / per_word : 0;
        nc_int(w, words);
        for (int word = 0, index = 0; word < words; word++) {
            uint64_t packed = 0;
            for (int j = 0; j < per_word && index < 4096; j++, index++) packed |= (uint64_t)indices[index] << (j * bits);
            nc_write(w, &packed, 8);
        }
    }
    if (!w->failed) memcpy(w->data + count_at, &section_count, 4);
    nc_int(w, diagnostic_count); nc_write(w, diagnostics, diagnostic_count * 4);
}

/* The direct transport belongs to this Java worker. Each callback consumes a complete frame
 * synchronously, so no borrowed native memory escapes into the cache. */
JNIEXPORT void JNICALL
Java_com_nullframe_core_cubiomes_Cubiomes_nativeStreamBuiltSurfaceRegionV2(JNIEnv *env, jclass clazz,
        jlong seed, jint dimension, jint x, jint z, jint width, jint height, jint step, jbyteArray mask_array,
        jboolean region_mode, jint bottom, jint top, jboolean fill_bottom, jboolean vegetation, jint surface_depth,
        jobject output, jobject receiver) {
    (void)clazz;
    int count = width * height;
    if (width < 1 || height < 1 || width > 32 || height > 32 || count > 256
            || bottom % 16 || top % 16 || top <= bottom || (int64_t)top - bottom > 4096
            || bottom < -1000000 || top > 1000000 || surface_depth < -1 || surface_depth > 4096 || step < 1 || step > 128 || (step & (step - 1))
            || (mask_array && (*env)->GetArrayLength(env, mask_array) != count)) {
        jclass error = (*env)->FindClass(env, "java/lang/IllegalArgumentException");
        if (error) (*env)->ThrowNew(env, error, "Invalid native surface region");
        return;
    }
    unsigned char *address = output ? (unsigned char *)(*env)->GetDirectBufferAddress(env, output) : NULL;
    jlong capacity = output ? (*env)->GetDirectBufferCapacity(env, output) : 0;
    if (!address || capacity < 1048576 || !receiver) {
        jclass error = (*env)->FindClass(env, "java/lang/IllegalArgumentException");
        if (error) (*env)->ThrowNew(env, error, "Missing worker direct chunk buffer");
        return;
    }
    jclass receiver_class = (*env)->GetObjectClass(env, receiver);
    if (!receiver_class) return;
    jmethodID complete = (*env)->GetMethodID(env, receiver_class, "onChunk", "(II)V");
    (*env)->DeleteLocalRef(env, receiver_class);
    if (!complete) return;
    jbyte mask[256]; memset(mask, 1, (size_t)count);
    if (mask_array) (*env)->GetByteArrayRegion(env, mask_array, 0, count, mask);
    if ((*env)->ExceptionCheck(env)) return;
    int *snapshots = (int *)calloc((size_t)count * PACKED_CHUNK_INTS, sizeof(int));
    uint16_t *blocks = (uint16_t *)malloc((size_t)(top - bottom) * 256 * sizeof(uint16_t));
    int *veg = vegetation ? (int *)malloc(VEG_BUFFER_INTS * sizeof(int)) : NULL;
    if (!snapshots || !blocks || (vegetation && !veg)) {
        jclass error = (*env)->FindClass(env, "java/lang/OutOfMemoryError");
        if (error) (*env)->ThrowNew(env, error, "Native streamed chunk allocation failed");
        goto stream_cleanup;
    }
    int dim = normalize_dimension_id(dimension);
    if (region_mode) myelin_get_surface_region_data_masked(seed, dim, x, z, width, height, step,
            (unsigned char *)mask, snapshots);
    NativeChunkWriter writer = {address, 0, 1048576, 0, 1};
    for (int i = 0; i < count; i++) {
        if (!mask[i]) continue;
        int *snapshot = snapshots + i * PACKED_CHUNK_INTS;
        if (!region_mode) myelin_get_surface_chunk_data(seed, dim, x + i % width, z + i / width, snapshot, step);
        if (dim == CB_DIM_NETHER) for (int j = 0; j < 256; j++) {
            snapshot[j] = 127; snapshot[256 + j] = SURFACE_STONE; snapshot[512 + j] = 1;
        }
        nc_build_blocks(blocks, snapshot, dim, bottom, top, fill_bottom, vegetation, surface_depth, seed, x + i % width, z + i / width, veg);
        writer.size = 0;
        nc_encode(&writer, blocks, snapshot, bottom, top, fill_bottom, vegetation, surface_depth);
        if (writer.failed) {
            jclass error = (*env)->FindClass(env, "java/lang/IllegalStateException");
            if (error) (*env)->ThrowNew(env, error, "Native chunk exceeded worker direct buffer");
            break;
        }
        (*env)->CallVoidMethod(env, receiver, complete, i, writer.size);
        if ((*env)->ExceptionCheck(env)) break;
    }
stream_cleanup:
    free(veg); free(blocks); free(snapshots);
}

JNIEXPORT jobjectArray JNICALL
Java_com_nullframe_core_cubiomes_Cubiomes_nativeGetBuiltSurfaceRegionV2(JNIEnv *env, jclass clazz,
        jlong seed, jint dimension, jint x, jint z, jint width, jint height, jint step, jbyteArray mask_array,
        jboolean region_mode, jint bottom, jint top, jboolean fill_bottom, jboolean vegetation, jint surface_depth) {
    (void)clazz;
    int count = width * height;
    if (width < 1 || height < 1 || width > 32 || height > 32 || count > 256
            || bottom % 16 || top % 16 || top <= bottom || (int64_t)top - bottom > 4096
            || bottom < -1000000 || top > 1000000 || surface_depth < -1 || surface_depth > 4096 || step < 1 || step > 128 || (step & (step - 1))
            || (mask_array && (*env)->GetArrayLength(env, mask_array) != count)) {
        jclass error = (*env)->FindClass(env, "java/lang/IllegalArgumentException");
        if (error) (*env)->ThrowNew(env, error, "Invalid native surface region");
        return NULL;
    }
    jbyte mask[256]; memset(mask, 1, (size_t)count);
    if (mask_array) (*env)->GetByteArrayRegion(env, mask_array, 0, count, mask);
    if ((*env)->ExceptionCheck(env)) return NULL;
    int *snapshots = (int *)calloc((size_t)count * PACKED_CHUNK_INTS, sizeof(int));
    uint16_t *blocks = (uint16_t *)malloc((size_t)(top - bottom) * 256 * sizeof(uint16_t));
    int *veg = vegetation ? (int *)malloc(VEG_BUFFER_INTS * sizeof(int)) : NULL;
    NativeChunkWriter writer = {0};
    jobjectArray result = NULL;
    if (!snapshots || !blocks || (vegetation && !veg)) goto out_of_memory;
    int dim = normalize_dimension_id(dimension);
    if (region_mode) myelin_get_surface_region_data_masked(seed, dim, x, z, width, height, step,
            (unsigned char *)mask, snapshots);
    jclass bytes_class = (*env)->FindClass(env, "[B");
    if (!bytes_class) goto cleanup;
    result = (*env)->NewObjectArray(env, count, bytes_class, NULL);
    (*env)->DeleteLocalRef(env, bytes_class);
    if (!result) goto cleanup;
    for (int i = 0; i < count; i++) {
        if (!mask[i]) continue;
        int *snapshot = snapshots + i * PACKED_CHUNK_INTS;
        if (!region_mode) myelin_get_surface_chunk_data(seed, dim, x + i % width, z + i / width, snapshot, step);
        /* Keep the existing direct API's deliberate Nether placeholder. */
        if (dim == CB_DIM_NETHER) for (int j = 0; j < 256; j++) {
            snapshot[j] = 127; snapshot[256 + j] = SURFACE_STONE; snapshot[512 + j] = 1;
        }
        nc_build_blocks(blocks, snapshot, dim, bottom, top, fill_bottom, vegetation, surface_depth, seed, x + i % width, z + i / width, veg);
        writer.size = 0;
        nc_encode(&writer, blocks, snapshot, bottom, top, fill_bottom, vegetation, surface_depth);
        if (writer.failed) goto out_of_memory;
        jbyteArray bytes = (*env)->NewByteArray(env, writer.size);
        if (!bytes) goto cleanup;
        (*env)->SetByteArrayRegion(env, bytes, 0, writer.size, (jbyte *)writer.data);
        (*env)->SetObjectArrayElement(env, result, i, bytes);
        (*env)->DeleteLocalRef(env, bytes);
        if ((*env)->ExceptionCheck(env)) goto cleanup;
    }
    goto cleanup;
out_of_memory: {
        jclass error = (*env)->FindClass(env, "java/lang/OutOfMemoryError");
        if (error) (*env)->ThrowNew(env, error, "Native combined chunk allocation failed");
    }
cleanup:
    free(writer.data); free(veg); free(blocks); free(snapshots);
    return (*env)->ExceptionCheck(env) ? NULL : result;
}

JNIEXPORT jbyteArray JNICALL
Java_com_nullframe_core_cubiomes_Cubiomes_nativeBuildSurfaceSnapshotV2(JNIEnv *env, jclass clazz,
        jlong seed, jint dimension, jint x, jint z, jintArray columns, jint bottom, jint top,
        jboolean fill_bottom, jboolean vegetation, jint surface_depth) {
    (void)clazz;
    if (!columns || (*env)->GetArrayLength(env, columns) != 768 || bottom % 16 || top % 16
            || top <= bottom || (int64_t)top - bottom > 4096 || bottom < -1000000 || top > 1000000
            || surface_depth < -1 || surface_depth > 4096) {
        jclass error = (*env)->FindClass(env, "java/lang/IllegalArgumentException");
        if (error) (*env)->ThrowNew(env, error, "Invalid native surface snapshot");
        return NULL;
    }
    int snapshot[768];
    (*env)->GetIntArrayRegion(env, columns, 0, 768, snapshot);
    if ((*env)->ExceptionCheck(env)) return NULL;
    uint16_t *blocks = (uint16_t *)malloc((size_t)(top - bottom) * 256 * sizeof(uint16_t));
    int *veg = vegetation ? (int *)malloc(VEG_BUFFER_INTS * sizeof(int)) : NULL;
    NativeChunkWriter writer = {0};
    jbyteArray result = NULL;
    if (blocks && (!vegetation || veg)) {
        nc_build_blocks(blocks, snapshot, normalize_dimension_id(dimension), bottom, top,
                fill_bottom, vegetation, surface_depth, seed, x, z, veg);
        nc_encode(&writer, blocks, snapshot, bottom, top, fill_bottom, vegetation, surface_depth);
        if (!writer.failed) {
            result = (*env)->NewByteArray(env, writer.size);
            if (result) (*env)->SetByteArrayRegion(env, result, 0, writer.size, (jbyte *)writer.data);
        }
    }
    free(writer.data); free(veg); free(blocks);
    if (!result && !(*env)->ExceptionCheck(env)) {
        jclass error = (*env)->FindClass(env, "java/lang/OutOfMemoryError");
        if (error) (*env)->ThrowNew(env, error, "Native snapshot build allocation failed");
    }
    return result;
}
