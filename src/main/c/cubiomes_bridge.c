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

#include <jni.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "util.h"
#include "surface_generation.h"
#include "surface_classification.h"
#include "terrain_sampling.h"
#include "terrain_math.h"
#include "vegetation.h"

static jint to_jint(int value) { return (jint) value; }

JNIEXPORT void JNICALL Java_com_nullframe_core_cubiomes_Cubiomes_nativeSetCorrectionAmp(
        JNIEnv *env, jclass clazz, jint amp) {
    (void)env; (void)clazz;
    if (amp < 0) amp = 0;
    if (amp > 15) amp = 15;
    g_correction_amp = amp;
}

JNIEXPORT void JNICALL Java_com_nullframe_core_cubiomes_Cubiomes_nativeSetBiomeStep(
        JNIEnv *env, jclass clazz, jint step) {
    (void)env; (void)clazz;
    if (step < 0) step = 0;
    if (step > 128) step = 128;
    g_biome_step = step;
}

JNIEXPORT jstring JNICALL Java_com_nullframe_core_cubiomes_Cubiomes_nativeGetBiomeName(
        JNIEnv *env, jclass clazz, jint biomeId) {
    (void)clazz;
    const char *name = biome2str(MC_NEWEST, (int)biomeId);
    if (name == NULL) return NULL;
    return (*env)->NewStringUTF(env, name);
}

/* JNI wrappers */

JNIEXPORT void JNICALL
Java_com_nullframe_core_cubiomes_Cubiomes_nativeGetLodGrid(
        JNIEnv *env, jclass clazz,
        jlong seed, jint cellSizeBlocks,
        jint minBlockX, jint minBlockZ,
        jint gridWidth, jint gridHeight,
        jint samplingStep,
        jintArray outHeights, jintArray outBiomes, jintArray outTypes) {
    (void) clazz;
    jsize count = (outHeights != NULL) ? (*env)->GetArrayLength(env, outHeights) : 0;
    if (count <= 0 || outBiomes == NULL || outTypes == NULL) return;
    if ((*env)->GetArrayLength(env, outBiomes) != count ||
        (*env)->GetArrayLength(env, outTypes) != count) return;
    jint *heights = (*env)->GetIntArrayElements(env, outHeights, NULL);
    jint *biomes = (*env)->GetIntArrayElements(env, outBiomes, NULL);
    jint *types = (*env)->GetIntArrayElements(env, outTypes, NULL);
    if (heights == NULL || biomes == NULL || types == NULL) {
        if (heights) (*env)->ReleaseIntArrayElements(env, outHeights, heights, JNI_ABORT);
        if (biomes) (*env)->ReleaseIntArrayElements(env, outBiomes, biomes, JNI_ABORT);
        if (types) (*env)->ReleaseIntArrayElements(env, outTypes, types, JNI_ABORT);
        return;
    }
    myelin_get_lod_grid((long long) seed, (int) cellSizeBlocks,
                            (int) minBlockX, (int) minBlockZ,
                            (int) gridWidth, (int) gridHeight,
                            (int) samplingStep,
                            (int *) heights, (int *) biomes, (int *) types);
    (*env)->ReleaseIntArrayElements(env, outHeights, heights, 0);
    (*env)->ReleaseIntArrayElements(env, outBiomes, biomes, 0);
    (*env)->ReleaseIntArrayElements(env, outTypes, types, 0);
}

JNIEXPORT jint JNICALL
Java_com_nullframe_core_cubiomes_Cubiomes_nativeGetTerrainHeight(
        JNIEnv *env, jclass clazz,
        jlong seed, jint dimensionId, jint blockX, jint blockZ) {
    (void) env;
    (void) clazz;
    int safeDim = normalize_dimension_id((int) dimensionId);
    return to_jint(myelin_get_terrain_height(
            (long long) seed, safeDim, (int) blockX, (int) blockZ));
}

/*
 * Quad-baker JNI entry points. Capacity calculation counts up to 13 quads per cell.
 * The bake call generates surface data and writes quad records into Java's direct buffer.
 */

#include "quad_baker.h"

static int qb_floor_div_pow2_jni(int a, int b) {
    if (a >= 0) return a / b;
    return -((-a + b - 1) / b);
}

static int qb_abs_jni(int x) { return x < 0 ? -x : x; }

/* Mirror of qb_bake_region's safe-cell iteration. Near 1x1 cells can emit a
 * top plus up to four layered exposed walls (13 quads worst case). */
static int quad_bake_capacity(int regionW, int regionH,
                              int originX, int originZ,
                              int centerX, int centerZ,
                              const QbTier *tiers, int tierCount) {
    if (regionW <= 0 || regionH <= 0 || tierCount < 1 || tierCount > QB_MAX_TIERS) return -1;
    const int bw = regionW * 16, bh = regionH * 16;
    int cells = 0;
    int prev_max = 0;
    for (int t = 0; t < tierCount; t++) {
        const int requested_s = tiers[t].cell_blocks;
        const int S = requested_s > 16 ? 16 : requested_s;
        const int maxd = tiers[t].max_dist;
        if (requested_s <= 0 || S <= 0 || maxd <= prev_max) { prev_max = maxd; continue; }
        int gx0 = qb_floor_div_pow2_jni(originX, S) * S;
        int gz0 = qb_floor_div_pow2_jni(originZ, S) * S;
        int gx1 = qb_floor_div_pow2_jni(originX + bw - 1, S) * S;
        int gz1 = qb_floor_div_pow2_jni(originZ + bh - 1, S) * S;
        for (int wz = gz0; wz <= gz1; wz += S) {
            for (int wx = gx0; wx <= gx1; wx += S) {
                int ccx = wx + S / 2, ccz = wz + S / 2;
                int dx = qb_abs_jni(ccx - centerX), dz = qb_abs_jni(ccz - centerZ);
                int dist = dx > dz ? dx : dz;
                if (dist < prev_max || dist >= maxd) continue;
                int lx0 = wx - originX, lz0 = wz - originZ;
                if (lx0 >= bw || lz0 >= bh || lx0 + S <= 0 || lz0 + S <= 0) continue;
                cells++;
            }
        }
        prev_max = maxd;
    }
    return cells * 13;
}

JNIEXPORT jint JNICALL
Java_com_nullframe_core_cubiomes_Cubiomes_nativeQuadBakeCapacity(
        JNIEnv *env, jclass clazz,
        jint regionChunkX, jint regionChunkZ,
        jint regionWidth, jint regionHeight,
        jint centerBlockX, jint centerBlockZ,
        jintArray tierBlocks, jintArray tierMaxDists) {
    (void) clazz;
    jsize count = (tierBlocks != NULL) ? (*env)->GetArrayLength(env, tierBlocks) : 0;
    if (count <= 0 || tierMaxDists == NULL ||
        (*env)->GetArrayLength(env, tierMaxDists) != count) return -1;
    if (count > QB_MAX_TIERS) count = QB_MAX_TIERS;
    jint *blocks = (*env)->GetIntArrayElements(env, tierBlocks, NULL);
    jint *maxd = (*env)->GetIntArrayElements(env, tierMaxDists, NULL);
    if (blocks == NULL || maxd == NULL) {
        if (blocks) (*env)->ReleaseIntArrayElements(env, tierBlocks, blocks, JNI_ABORT);
        if (maxd) (*env)->ReleaseIntArrayElements(env, tierMaxDists, maxd, JNI_ABORT);
        return -1;
    }
    QbTier tiers[QB_MAX_TIERS];
    for (int i = 0; i < (int) count; i++) {
        tiers[i].cell_blocks = (int) blocks[i];
        tiers[i].max_dist = (int) maxd[i];
    }
    int cap = quad_bake_capacity((int) regionWidth, (int) regionHeight,
                                 (int) regionChunkX * 16, (int) regionChunkZ * 16,
                                 (int) centerBlockX, (int) centerBlockZ,
                                 tiers, (int) count);
    (*env)->ReleaseIntArrayElements(env, tierBlocks, blocks, JNI_ABORT);
    (*env)->ReleaseIntArrayElements(env, tierMaxDists, maxd, JNI_ABORT);
    return (jint) cap;
}

JNIEXPORT jint JNICALL
Java_com_nullframe_core_cubiomes_Cubiomes_nativeBakeRegionQuads(
        JNIEnv *env, jclass clazz,
        jlong seed, jint dimensionId,
        jint regionChunkX, jint regionChunkZ,
        jint regionWidth, jint regionHeight,
        jint samplingStep,
        jint centerBlockX, jint centerBlockZ,
        jintArray tierBlocks, jintArray tierMaxDists,
        jobject outBuffer) {
    (void) clazz;
    if (outBuffer == NULL) return -1;
    jsize count = (tierBlocks != NULL) ? (*env)->GetArrayLength(env, tierBlocks) : 0;
    if (count <= 0 || count > QB_MAX_TIERS || tierMaxDists == NULL ||
        (*env)->GetArrayLength(env, tierMaxDists) != count) return -1;
    jint *blocks = (*env)->GetIntArrayElements(env, tierBlocks, NULL);
    jint *maxd = (*env)->GetIntArrayElements(env, tierMaxDists, NULL);
    if (blocks == NULL || maxd == NULL) {
        if (blocks) (*env)->ReleaseIntArrayElements(env, tierBlocks, blocks, JNI_ABORT);
        if (maxd) (*env)->ReleaseIntArrayElements(env, tierMaxDists, maxd, JNI_ABORT);
        return -1;
    }
    QbTier tiers[QB_MAX_TIERS];
    for (int i = 0; i < (int) count; i++) {
        tiers[i].cell_blocks = (int) blocks[i];
        tiers[i].max_dist = (int) maxd[i];
    }

    int rw = (int) regionWidth, rh = (int) regionHeight;
    if (rw < 1) rw = 1;
    if (rh < 1) rh = 1;
    int safeDim = normalize_dimension_id((int) dimensionId);
    int chunkCount = rw * rh;

    /* Region surface data is chunk-interleaved: [heights][types][biomes] per chunk. */
    int *region = (int *) malloc(sizeof(int) * (size_t) chunkCount * PACKED_CHUNK_INTS);
    if (region == NULL) {
        (*env)->ReleaseIntArrayElements(env, tierBlocks, blocks, JNI_ABORT);
        (*env)->ReleaseIntArrayElements(env, tierMaxDists, maxd, JNI_ABORT);
        return -1;
    }
    myelin_get_surface_region_data((long long) seed, safeDim,
                                        (int) regionChunkX, (int) regionChunkZ,
                                        rw, rh, (int) samplingStep, region);

    int cap = quad_bake_capacity(rw, rh,
                                 (int) regionChunkX * 16, (int) regionChunkZ * 16,
                                 (int) centerBlockX, (int) centerBlockZ,
                                 tiers, (int) count);
    jlong bufCap = (*env)->GetDirectBufferCapacity(env, outBuffer);
    if (cap < 0 || bufCap < (jlong) cap * (jlong) sizeof(QbQuad)) {
        free(region);
        (*env)->ReleaseIntArrayElements(env, tierBlocks, blocks, JNI_ABORT);
        (*env)->ReleaseIntArrayElements(env, tierMaxDists, maxd, JNI_ABORT);
        return -2;  /* buffer too small — caller must allocate cap * sizeof(QbQuad) */
    }

    QbQuad *quads = (QbQuad *) (*env)->GetDirectBufferAddress(env, outBuffer);
    if (quads == NULL) {
        free(region);
        (*env)->ReleaseIntArrayElements(env, tierBlocks, blocks, JNI_ABORT);
        (*env)->ReleaseIntArrayElements(env, tierMaxDists, maxd, JNI_ABORT);
        return -1;
    }

    int out_count = 0;
    int tier_counts[QB_MAX_TIERS] = {0};
    int rc = qb_bake_region_interleaved(region, rw, rh,
                            (int) regionChunkX * 16, (int) regionChunkZ * 16,
                            (int) centerBlockX, (int) centerBlockZ,
                            tiers, (int) count,
                            quads, cap, &out_count, tier_counts);
    free(region);
    (*env)->ReleaseIntArrayElements(env, tierBlocks, blocks, JNI_ABORT);
    (*env)->ReleaseIntArrayElements(env, tierMaxDists, maxd, JNI_ABORT);
    if (rc == -2) return -2;  /* should not happen (capacity = exact upper bound) */
    if (rc != 0) return -1;
    return (jint) out_count;
}

JNIEXPORT jintArray JNICALL
Java_com_nullframe_core_cubiomes_Cubiomes_nativeGetSurfaceChunkData(
        JNIEnv *env, jclass clazz,
        jlong seed, jint dimensionId, jint chunkX, jint chunkZ) {
    (void) clazz;

    int packed[PACKED_CHUNK_INTS];
    int safeDim = normalize_dimension_id((int) dimensionId);
    myelin_get_surface_chunk_data((long long) seed, safeDim,
                                      (int) chunkX, (int) chunkZ, packed, 1);

    jintArray result = (*env)->NewIntArray(env, PACKED_CHUNK_INTS);
    if (result == NULL) return NULL;
    (*env)->SetIntArrayRegion(env, result, 0, PACKED_CHUNK_INTS, (const jint *) packed);
    return result;
}

JNIEXPORT jintArray JNICALL
Java_com_nullframe_core_cubiomes_Cubiomes_nativeGetRegionSurfaceData(
        JNIEnv *env, jclass clazz,
        jlong seed, jint dimensionId,
        jint regionChunkX, jint regionChunkZ,
        jint regionWidth, jint regionHeight) {
    (void) clazz;

    int safeWidth = (int) regionWidth;
    int safeHeight = (int) regionHeight;
    if (safeWidth < 1) safeWidth = 1;
    if (safeHeight < 1) safeHeight = 1;
    if (safeWidth > 8) safeWidth = 8;
    if (safeHeight > 8) safeHeight = 8;

    int packed_length = safeWidth * safeHeight * PACKED_CHUNK_INTS;
    int *packed = (int *) malloc(sizeof(int) * (size_t) packed_length);
    if (packed == NULL) return NULL;

    int safeDim = normalize_dimension_id((int) dimensionId);
    myelin_get_region_surface_data(
            (long long) seed, safeDim,
            (int) regionChunkX, (int) regionChunkZ,
            safeWidth, safeHeight, packed);

    jintArray result = (*env)->NewIntArray(env, packed_length);
    if (result == NULL) {
        free(packed);
        return NULL;
    }
    (*env)->SetIntArrayRegion(env, result, 0, packed_length, (const jint *) packed);
    free(packed);
    return result;
}

/*
 * Generate vegetation from 256 heights, biomes and surface types.
 * Output is [count, localX, worldY, localZ, materialId, ...].
 */

JNIEXPORT jintArray JNICALL
Java_com_nullframe_core_cubiomes_Cubiomes_nativeGetVegetation(
        JNIEnv *env, jclass clazz,
        jlong seed, jint chunkX, jint chunkZ,
        jintArray heightsArr, jintArray biomesArr, jintArray surfaceTypesArr,
        jint bottomY, jint topYExclusive) {
    (void) clazz;

    if (heightsArr == NULL || biomesArr == NULL || surfaceTypesArr == NULL) {
        return NULL;
    }

    jsize heights_len = (*env)->GetArrayLength(env, heightsArr);
    jsize biomes_len = (*env)->GetArrayLength(env, biomesArr);
    jsize types_len = (*env)->GetArrayLength(env, surfaceTypesArr);
    if (heights_len != CHUNK_COLUMN_COUNT || biomes_len != CHUNK_COLUMN_COUNT ||
        types_len != CHUNK_COLUMN_COUNT) {
        return NULL;
    }

    jint *heights = (*env)->GetIntArrayElements(env, heightsArr, NULL);
    jint *biomes = (*env)->GetIntArrayElements(env, biomesArr, NULL);
    jint *surface_types = (*env)->GetIntArrayElements(env, surfaceTypesArr, NULL);
    if (heights == NULL || biomes == NULL || surface_types == NULL) {
        if (heights) (*env)->ReleaseIntArrayElements(env, heightsArr, heights, JNI_ABORT);
        if (biomes) (*env)->ReleaseIntArrayElements(env, biomesArr, biomes, JNI_ABORT);
        if (surface_types) (*env)->ReleaseIntArrayElements(env, surfaceTypesArr, surface_types, JNI_ABORT);
        return NULL;
    }

    int out_int_count = 0;
    int *veg_data = myelin_get_vegetation(
            (long long) seed, (int) chunkX, (int) chunkZ,
            (const int *) heights, (const int *) biomes, (const int *) surface_types,
            (int) bottomY, (int) topYExclusive, &out_int_count);

    (*env)->ReleaseIntArrayElements(env, heightsArr, heights, JNI_ABORT);
    (*env)->ReleaseIntArrayElements(env, biomesArr, biomes, JNI_ABORT);
    (*env)->ReleaseIntArrayElements(env, surfaceTypesArr, surface_types, JNI_ABORT);

    if (veg_data == NULL || out_int_count <= 0) {
        free(veg_data);
        /* Return a 1-element array with count=0 */
        jintArray empty = (*env)->NewIntArray(env, 1);
        if (empty == NULL) return NULL;
        jint zero = 0;
        (*env)->SetIntArrayRegion(env, empty, 0, 1, &zero);
        return empty;
    }

    jintArray result = (*env)->NewIntArray(env, out_int_count);
    if (result == NULL) {
        free(veg_data);
        return NULL;
    }
    (*env)->SetIntArrayRegion(env, result, 0, out_int_count, (const jint *) veg_data);
    free(veg_data);
    return result;
}

/* Batched vegetation for region strips. Input arrays are flattened
 * [count × 256] height/biome/surfaceType arrays + parallel coord arrays.
 * Returns [count, (vegCount, x,y,z,type × vegCount) × count]. */
JNIEXPORT jintArray JNICALL
Java_com_nullframe_core_cubiomes_Cubiomes_nativeGetRegionVegetation(
        JNIEnv *env, jclass clazz,
        jlong seed,
        jintArray chunkXsArr, jintArray chunkZsArr,
        jintArray flatHeightsArr, jintArray flatBiomesArr, jintArray flatSurfaceTypesArr,
        jint bottomY, jint topYExclusive) {
    (void) clazz;

    jsize count = (chunkXsArr != NULL) ? (*env)->GetArrayLength(env, chunkXsArr) : 0;
    if (count <= 0 || chunkZsArr == NULL) return NULL;
    jsize expected_flat = count * CHUNK_COLUMN_COUNT;
    if ((*env)->GetArrayLength(env, chunkZsArr) != count ||
        (*env)->GetArrayLength(env, flatHeightsArr) != expected_flat ||
        (*env)->GetArrayLength(env, flatBiomesArr) != expected_flat ||
        (*env)->GetArrayLength(env, flatSurfaceTypesArr) != expected_flat) {
        return NULL;
    }

    jint *xs = (*env)->GetIntArrayElements(env, chunkXsArr, NULL);
    jint *zs = (*env)->GetIntArrayElements(env, chunkZsArr, NULL);
    jint *heights = (*env)->GetIntArrayElements(env, flatHeightsArr, NULL);
    jint *biomes = (*env)->GetIntArrayElements(env, flatBiomesArr, NULL);
    jint *types = (*env)->GetIntArrayElements(env, flatSurfaceTypesArr, NULL);
    if (xs == NULL || zs == NULL || heights == NULL || biomes == NULL || types == NULL) {
        if (xs) (*env)->ReleaseIntArrayElements(env, chunkXsArr, xs, JNI_ABORT);
        if (zs) (*env)->ReleaseIntArrayElements(env, chunkZsArr, zs, JNI_ABORT);
        if (heights) (*env)->ReleaseIntArrayElements(env, flatHeightsArr, heights, JNI_ABORT);
        if (biomes) (*env)->ReleaseIntArrayElements(env, flatBiomesArr, biomes, JNI_ABORT);
        if (types) (*env)->ReleaseIntArrayElements(env, flatSurfaceTypesArr, types, JNI_ABORT);
        return NULL;
    }

    int out_int_count = 0;
    int *veg_data = myelin_get_region_vegetation(
            (long long) seed, (int) count,
            (const int *) xs, (const int *) zs,
            (const int *) heights, (const int *) biomes, (const int *) types,
            (int) bottomY, (int) topYExclusive, &out_int_count);

    (*env)->ReleaseIntArrayElements(env, chunkXsArr, xs, JNI_ABORT);
    (*env)->ReleaseIntArrayElements(env, chunkZsArr, zs, JNI_ABORT);
    (*env)->ReleaseIntArrayElements(env, flatHeightsArr, heights, JNI_ABORT);
    (*env)->ReleaseIntArrayElements(env, flatBiomesArr, biomes, JNI_ABORT);
    (*env)->ReleaseIntArrayElements(env, flatSurfaceTypesArr, types, JNI_ABORT);

    if (veg_data == NULL || out_int_count <= 0) {
        return NULL;
    }
    jintArray result = (*env)->NewIntArray(env, out_int_count);
    if (result == NULL) return NULL;
    (*env)->SetIntArrayRegion(env, result, 0, out_int_count, (const jint *) veg_data);
    return result;
}

/*
 * Direct-buffer surface transport. C writes 768 ints per chunk:
 * 256 heights, 256 surface types and 256 biome IDs.
 * Region methods write chunks consecutively; Java then copies values into snapshots.
 */

JNIEXPORT void JNICALL
Java_com_nullframe_core_cubiomes_Cubiomes_nativeGetSurfaceChunkDataDirect(
        JNIEnv *env, jclass clazz,
        jlong seed, jint dimensionId, jint chunkX, jint chunkZ,
        jint samplingStep, jobject directBuffer) {
    (void) clazz;

    if (directBuffer == NULL) return;

    /* Get direct pointer to Java's off-heap buffer — NO COPY */
    int *data = (int *) (*env)->GetDirectBufferAddress(env, directBuffer);
    if (data == NULL) return;

    int safeDim = normalize_dimension_id((int) dimensionId);

    /* Generate directly into the buffer — no intermediate array */
    myelin_get_surface_chunk_data(
            (long long) seed, safeDim,
            (int) chunkX, (int) chunkZ,
            data, (int) samplingStep);
}

JNIEXPORT void JNICALL
Java_com_nullframe_core_cubiomes_Cubiomes_nativeGetSurfaceRegionDataDirect(
        JNIEnv *env, jclass clazz,
        jlong seed, jint dimensionId,
        jint regionChunkX, jint regionChunkZ,
        jint regionWidth, jint regionHeight,
        jint samplingStep, jobject directBuffer) {
    (void) clazz;

    if (directBuffer == NULL) return;

    int *data = (int *) (*env)->GetDirectBufferAddress(env, directBuffer);
    if (data == NULL) return;

    int safeDim = normalize_dimension_id((int) dimensionId);
    int safeW = (int) regionWidth;
    int safeH = (int) regionHeight;
    if (safeW < 1) safeW = 1;
    if (safeH < 1) safeH = 1;

    /* Generate the region into the caller-owned buffer in one native call. */
    myelin_get_surface_region_data(
            (long long) seed, safeDim,
            (int) regionChunkX, (int) regionChunkZ,
            safeW, safeH,
            (int) samplingStep, data);
}

/* Region variant with a compute mask: only mask[idx] != 0 chunks are
 * generated (skips already-cached/pending chunks inside the rectangle). */
JNIEXPORT void JNICALL
Java_com_nullframe_core_cubiomes_Cubiomes_nativeGetSurfaceRegionDataDirectMasked(
        JNIEnv *env, jclass clazz,
        jlong seed, jint dimensionId,
        jint regionChunkX, jint regionChunkZ,
        jint regionWidth, jint regionHeight,
        jint samplingStep,
        jbyteArray maskArr, jobject directBuffer) {
    (void) clazz;

    if (directBuffer == NULL) return;

    int *data = (int *) (*env)->GetDirectBufferAddress(env, directBuffer);
    if (data == NULL) return;

    int safeDim = normalize_dimension_id((int) dimensionId);
    int safeW = (int) regionWidth;
    int safeH = (int) regionHeight;
    if (safeW < 1) safeW = 1;
    if (safeH < 1) safeH = 1;

    unsigned char *mask = NULL;
    jbyte *mask_elems = NULL;
    if (maskArr != NULL) {
        jsize mask_len = (*env)->GetArrayLength(env, maskArr);
        if (mask_len == (jsize) (safeW * safeH)) {
            mask_elems = (*env)->GetByteArrayElements(env, maskArr, NULL);
            mask = (unsigned char *) mask_elems;
        }
    }

    myelin_get_surface_region_data_masked(
            (long long) seed, safeDim,
            (int) regionChunkX, (int) regionChunkZ,
            safeW, safeH,
            (int) samplingStep, mask, data);

    if (mask_elems != NULL) {
        (*env)->ReleaseByteArrayElements(env, maskArr, mask_elems, JNI_ABORT);
    }
}

JNIEXPORT void JNICALL
Java_com_nullframe_core_cubiomes_Cubiomes_nativeGetRegionSurfaceDataDirect(
        JNIEnv *env, jclass clazz,
        jlong seed, jint dimensionId,
        jint regionChunkX, jint regionChunkZ,
        jint regionWidth, jint regionHeight,
        jobject directBuffer) {
    (void) clazz;

    if (directBuffer == NULL) return;

    int *data = (int *) (*env)->GetDirectBufferAddress(env, directBuffer);
    if (data == NULL) return;

    int safeWidth = (int) regionWidth;
    int safeHeight = (int) regionHeight;
    if (safeWidth < 1) safeWidth = 1;
    if (safeHeight < 1) safeHeight = 1;
    if (safeWidth > 8) safeWidth = 8;
    if (safeHeight > 8) safeHeight = 8;

    int safeDim = normalize_dimension_id((int) dimensionId);

    /* Write legacy region output into the caller-owned buffer. */
    myelin_get_region_surface_data(
            (long long) seed, safeDim,
            (int) regionChunkX, (int) regionChunkZ,
            safeWidth, safeHeight,
            data);
}

#include "native_chunk.h"
#include "compact_columns.h"
