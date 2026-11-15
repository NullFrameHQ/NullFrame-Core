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

/*
 * AVX-512 interpolation kernels. Callers must check om_cpu_has_avx512() first.
 * Keep multiply and add operations separate to match the scalar and AVX2 paths.
 */

#include "avx512_kernels.h"
#include <immintrin.h>

/* Local copy of the quintic easing function used by terrain_math.h. */
static float om_quintic_fade(float t) {
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

/* CPU + OS feature detection */

#if defined(_MSC_VER)
#include <intrin.h>

static int om_detect_avx512(void) {
    int regs[4];
    __cpuid(regs, 0);
    if (regs[0] < 7) return 0;                     /* no leaf 7 → no AVX-512 */

    __cpuidex(regs, 1, 0);
    if (!(regs[2] & (1 << 27))) return 0;          /* OSXSAVE: XGETBV usable  */
    if (!(regs[2] & (1 << 28))) return 0;          /* AVX (library baseline)  */
    const unsigned long long xcr0 = _xgetbv(0);
    /* OS must save XMM+YMM (bits 1-2) AND opmask+ZMM_Hi256+Hi16_ZMM (5-7). */
    if ((xcr0 & 0xE6ULL) != 0xE6ULL) return 0;

    __cpuidex(regs, 7, 0);
    if (!(regs[1] & (1 << 16))) return 0;          /* AVX512F                 */
    return 1;
}
#else
static int om_detect_avx512(void) {
    /* GCC/Clang builtin also performs the OS XGETBV check internally. */
    return __builtin_cpu_supports("avx512f") ? 1 : 0;
}
#endif

int om_cpu_has_avx512(void) {
    /* Cache the feature probe result. This plain static value is not atomic. */
    static int cached = -1;
    int v = cached;
    if (v < 0) {
        v = om_detect_avx512();
        cached = v;
    }
    return v;
}

/* ── Kernel 1: 16x16 chunk heights + optional correction, 16 lanes/row ──
 *
 * One __m512 covers an entire 16-wide row, so this is 16 vector ops for the
 * whole chunk vs 32 on AVX2. Math order per element (identical to scalar):
 *   h  = a + b*tx
 *   n  = bilerp(corr00, corr10, corr01, corr11, fade_u, fv)
 *   h += (n*2 - 1) * amp
 */
void om_fill_chunk16_avx512(float *out_heights,
                            float h00, float h10, float h01, float h11,
                            float dh_x, float dh2_x,
                            float phase_x, float phase_z,
                            int sampling_step,
                            float eff_corr_amp,
                            float corr00, float corr10,
                            float corr01, float corr11) {
    float tx_arr[16], fu_arr[16];
    for (int lx = 0; lx < 16; lx++) {
        tx_arr[lx] = (phase_x + (float)lx) / (float)sampling_step;
        fu_arr[lx] = om_quintic_fade((float)lx / 16.0f);
    }
    const __m512 vtx     = _mm512_loadu_ps(tx_arr);
    const __m512 vfu     = _mm512_loadu_ps(fu_arr);
    const __m512 vtwo    = _mm512_set1_ps(2.0f);
    const __m512 vone    = _mm512_set1_ps(1.0f);
    const __m512 vcorr00 = _mm512_set1_ps(corr00);
    const __m512 vcorr10 = _mm512_set1_ps(corr10);
    const __m512 vcorr01 = _mm512_set1_ps(corr01);
    const __m512 vcorr11 = _mm512_set1_ps(corr11);
    const __m512 vamp    = _mm512_set1_ps(eff_corr_amp);

    for (int lz = 0; lz < 16; lz++) {
        const float tz = (phase_z + (float)lz) / (float)sampling_step;
        const float a  = h00 + (h01 - h00) * tz;
        const float b  = dh_x + (dh2_x - dh_x) * tz;
        const __m512 va  = _mm512_set1_ps(a);
        const __m512 vb  = _mm512_set1_ps(b);
        const __m512 vfv = _mm512_set1_ps(om_quintic_fade((float)lz / 16.0f));

        __m512 h = _mm512_add_ps(va, _mm512_mul_ps(vb, vtx)); /* a + b*tx */
        if (eff_corr_amp > 0.0f) {
            const __m512 hx0 = _mm512_add_ps(vcorr00,
                    _mm512_mul_ps(_mm512_sub_ps(vcorr10, vcorr00), vfu));
            const __m512 hx1 = _mm512_add_ps(vcorr01,
                    _mm512_mul_ps(_mm512_sub_ps(vcorr11, vcorr01), vfu));
            const __m512 n = _mm512_add_ps(hx0,
                    _mm512_mul_ps(_mm512_sub_ps(hx1, hx0), vfv));
            const __m512 delta = _mm512_mul_ps(
                    _mm512_sub_ps(_mm512_mul_ps(n, vtwo), vone), vamp);
            h = _mm512_add_ps(h, delta);
        }
        _mm512_storeu_ps(&out_heights[lz * 16], h);
    }
}

/*
 * Fill one cell row with a masked vector store. Cell widths are one through sixteen;
 * tx is the column offset inside the cell.
 */
void om_fill_cell_avx512(float *out_heights, int idx, int cell_w,
                         int sampling_step, float a, float b) {
    float tx_arr[16];
    for (int c = 0; c < 16; c++) {
        tx_arr[c] = (float)c / (float)sampling_step;
    }
    const __m512 h = _mm512_add_ps(
            _mm512_mul_ps(_mm512_set1_ps(b), _mm512_loadu_ps(tx_arr)),
            _mm512_set1_ps(a));
    const __mmask16 mask = (__mmask16)((1u << cell_w) - 1u);
    _mm512_mask_storeu_ps(&out_heights[idx], mask, h);
}
