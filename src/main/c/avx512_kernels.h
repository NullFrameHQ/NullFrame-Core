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

#ifndef OM_AVX512_KERNELS_H
#define OM_AVX512_KERNELS_H

/*
 * Optional AVX-512 kernels. Only call the fill routines after the CPU/OS probe succeeds.
 * The rest of the native build uses AVX2.
 */

/* 1 when CPU *and* OS both support the AVX-512 subset used here (F + masked
 * ops, incl. the XCR0 opmask/ZMM state check). Cached after first call. */
int om_cpu_has_avx512(void);

/*
 * Fill a 16 by 16 height grid, with optional correction, one vector per row.
 * Uses the same interpolation order as terrain_sampling.c.
 */
void om_fill_chunk16_avx512(float *out_heights,
                            float h00, float h10, float h01, float h11,
                            float dh_x, float dh2_x,
                            float phase_x, float phase_z,
                            int sampling_step,
                            float eff_corr_amp,
                            float corr00, float corr10,
                            float corr01, float corr11);

/* One interpolation-cell row: out[idx .. idx+cell_w) = a + b*(c/step) with c
 * the offset within the cell. Requires 1 <= cell_w <= 16. The tail is
 * handled with a masked store — no scalar cleanup loop. */
void om_fill_cell_avx512(float *out_heights, int idx, int cell_w,
                         int sampling_step, float a, float b);

#endif /* OM_AVX512_KERNELS_H */
