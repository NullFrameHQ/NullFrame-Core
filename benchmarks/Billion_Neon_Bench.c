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

/* Billion_Neon_Bench.exe — 1:128 ONLY · 1,000,000,000 chunks · 10 threads.
 *
 * Same internals as Billion_Chunks_Benchmark.exe:
 *   - links the native engine modules directly (no JNI, pure-C path)
 *   - myelin_get_surface_chunk_data(), coherent sweep (i%64 rows)
 *   - 10 pinned TIME_CRITICAL workers, InterlockedAdd64 progress counter
 *   - g_correction_amp = 4, MYELIN_BENCH_NO_STAIRCASE
 * ONE workload: sampling step 128 only, full 1B chunks (~2-10 min).
 *
 * UI (pure ANSI, zero deps, Win10+ Windows Terminal):
 *   - green + white palette, sharp yellow as the ONLY alarm color
 *     ("Only AVX2" tag, warnings); red reserved for hard failures.
 *   - telemetry stat box, single-line in-place progress bar
 *   - live box-drawing LINE graph (● ╱ ╲ ─) of chunks/sec with a
 *     two-phase autoscaling Y axis:
 *       phase 1 (first ~45 s): 0 -> max, shows the warmup ramp
 *       phase 2 (steady ~9-10M): re-windows to [min-5%, max+5%] of recent
 *         samples, min span 1M ch/s, so the flat plateau shows its jitter.
 *   - UTF-8 output (SetConsoleOutputCP 65001) + VT processing enabled.
 *
 * Optional argv[1] = chunk count override (>= 100000) for quick tests.
 * Pauses on Enter at exit so double-click launches keep the results.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <windows.h>
#include "surface_generation.h"
#include "terrain_sampling.h"
#include "terrain_math.h"

#define NEON_WORKERS 10
#define NEON_TARGET  1000000000LL       /* 1 billion chunks */
static long long NEON_RUN_TARGET = NEON_TARGET;

#define NEON_STEP        128            /* the ONLY sampling level */
#define NEON_REPAINT_SEC 0.25           /* UI refresh / graph sample   */
#define NEON_WARMUP_SEC  8.0            /* brief 0-anchored window only */
#define NEON_MIN_SPAN    300000.0       /* steady-state min Y span (zoomed) */
#define GR_W 48                          /* graph columns               */
#define GR_H 6                           /* graph rows                  */
#define HIST_MAX 512                    /* full-run history (finale)   */

/* ── Palette: white + green, sharp yellow as alarm, red = errors only ── */
#define C_RESET "\033[0m"
#define C_BOLD  "\033[1m"
#define C_GREEN "\033[92m"
#define C_WHITE "\033[97m"
#define C_GRAY  "\033[90m"
#define C_YELL  "\033[93m"
#define C_RED   "\033[91m"

static double neon_now(void) {
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}

/* Thousands separator. */
static void neon_fmt(long long v, char *buf, size_t n) {
    char raw[32];
    _snprintf_s(raw, sizeof(raw), _TRUNCATE, "%lld", v);
    int len = (int)strlen(raw);
    int pos = 0;
    for (int i = 0; i < len; i++) {
        if (i > 0 && (len - i) % 3 == 0 && pos < (int)n - 1) buf[pos++] = ',';
        if (pos < (int)n - 1) buf[pos++] = raw[i];
    }
    buf[pos] = '\0';
}

/* ── Line builder: tracks VISIBLE width (ANSI codes are zero-width) ── */
static char L[2048];
static int  Lw;
static void lclr(void) { L[0] = '\0'; Lw = 0; }
static int  uwidth(const char *s) {          /* codepoint count, ANSI-0 */
    int w = 0;
    while (*s) {
        if (*s == '\033') {                  /* escape sequence: zero width */
            s++;
            while (*s && !((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z'))) s++;
            if (*s) s++;
            continue;
        }
        if ((*s & 0xC0) != 0x80) w++;
        s++;
    }
    return w;
}
static void lput(const char *s) {
    size_t len = strlen(s);
    if (Lw + (int)len < (int)sizeof(L) - 1) { strcat_s(L, sizeof(L), s); Lw += uwidth(s); }
}
static void lcol(const char *c, const char *s) { lput(c); lput(s); lput(C_RESET); }

#define NEON_W_IN 60   /* interior width of the box */

static void flush_box(void) {            /* ║ content ·pad ║ */
    char pad[80];
    int p = NEON_W_IN - Lw; if (p < 0) p = 0;
    _snprintf_s(pad, sizeof(pad), _TRUNCATE, "%*s", p, "");
    printf("\033[K\xe2\x95\x91%s%s\xe2\x95\x91\n", L, pad);
}
static void box_top(void)    { printf("\033[K\xe2\x95\x94"); for (int i = 0; i < NEON_W_IN; i++) printf("\xe2\x95\x90"); printf("\xe2\x95\x97\n"); }
static void box_mid(void)    { printf("\033[K\xe2\x95\xa0"); for (int i = 0; i < NEON_W_IN; i++) printf("\xe2\x95\x90"); printf("\xe2\x95\xa3\n"); }
static void box_bot(void)    { printf("\033[K\xe2\x95\x9a"); for (int i = 0; i < NEON_W_IN; i++) printf("\xe2\x95\x90"); printf("\xe2\x95\x9d\n"); }
static void flush_plain(void){ char pad[80]; int p = NEON_W_IN + 2 - Lw; if (p < 0) p = 0;
                               _snprintf_s(pad, sizeof(pad), _TRUNCATE, "%*s", p, "");
                               printf("\033[K%s%s\n", L, pad); }

/* ── Live graph state ── */
static float g_samples[GR_W];            /* ring, left-shifted when full */
static int   g_n = 0;
static float g_hist[HIST_MAX];           /* decimated full-run history   */
static int   g_hist_n = 0;
static int   g_hist_div = 0;

static void push_sample(float v) {
    if (g_n < GR_W) g_samples[g_n++] = v;
    else { memmove(g_samples, g_samples + 1, sizeof(float) * (GR_W - 1)); g_samples[GR_W - 1] = v; }
    if (++g_hist_div >= 8) {             /* ~every 2 s keep for finale    */
        g_hist_div = 0;
        if (g_hist_n < HIST_MAX) g_hist[g_hist_n++] = v;
        else { for (int i = 0; i < HIST_MAX / 2; i++) g_hist[i] = (g_hist[2*i] + g_hist[2*i+1]) * 0.5f;
               g_hist_n = HIST_MAX / 2; g_hist[HIST_MAX - 1] = v; g_hist[HIST_MAX/2 - 1] = v; }
    }
}

/* UTF-8 glyph constants */
#define G_HLINE "\xe2\x94\x80"   /* ─ */
#define G_SE    "\xe2\x95\xb1"   /* ╱ */
#define G_SW    "\xe2\x95\xb2"   /* ╲ */
#define G_DOT   "\xe2\x97\x8f"   /* ● */

static const char *g_grid[GR_H][GR_W];

/* Render samples[0..n-1] into the grid with window [lo,hi].
 * DOTS ONLY — one ● per sample, no connecting lines. */
static void render_graph(const float *s, int n, double lo, double hi) {
    for (int r = 0; r < GR_H; r++) for (int c = 0; c < GR_W; c++) g_grid[r][c] = " ";
    if (n < 1 || hi <= lo) return;
    double span = hi - lo;
    int m = n < GR_W ? n : GR_W;
    for (int i = 0; i < m; i++) {
        double t = ((double)s[i] - lo) / span;             /* 0 = bottom */
        int r = GR_H - 1 - (int)(t * (GR_H - 1) + 0.5);
        if (r < 0) r = 0; if (r >= GR_H) r = GR_H - 1;
        g_grid[r][i] = G_DOT;
    }
}

static void draw_graph(double lo, double hi) {
    char lab[32];
    for (int r = 0; r < GR_H; r++) {
        double v = hi - (hi - lo) * (double)r / (double)(GR_H - 1);
        lclr(); lput(" ");
        _snprintf_s(lab, sizeof(lab), _TRUNCATE, "%5.2fM", v / 1e6);
        lcol(C_GRAY, lab);
        lcol(C_GRAY, " \xe2\x94\xa4");                       /* ┤ */
        for (int c = 0; c < GR_W; c++) {
            if (g_grid[r][c] == G_DOT) lcol(C_BOLD C_GREEN, G_DOT);
            else                       lcol(C_GREEN, g_grid[r][c]);
        }
        flush_box();
    }
}

/* ── Workers: identical to Billion_Chunks_Benchmark.c ── */
static void neon_pin(int core) {
    SetThreadAffinityMask(GetCurrentThread(), 1ULL << (core % 64));
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
}

typedef struct {
    long long seed;
    int step;
    long long from, to;
    int core;
    int *out;
    volatile long long *done;
} neon_slice;

static DWORD WINAPI neon_thread(LPVOID arg) {
    neon_slice *s = (neon_slice *)arg;
    neon_pin(s->core);
    long long local = 0;
    for (long long i = s->from; i < s->to; i++) {
        int cx = 1000000 + (int)(i % 64);
        int cz = 2000000 + (int)(i / 64);
        myelin_get_surface_chunk_data(s->seed, CB_DIM_OVERWORLD, cx, cz, s->out, s->step);
        if (++local >= 4096) {
            InterlockedAdd64((volatile long long *)s->done, local);
            local = 0;
        }
    }
    if (local > 0) InterlockedAdd64((volatile long long *)s->done, local);
    return 0;
}

/* ── UI helpers ── */
static void neon_enter_alt_screen(void) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode)) SetConsoleMode(h, mode | 0x0004 /*VT*/);
    SetConsoleOutputCP(65001);
    printf("\033[2J\033[H\033[?25l");    /* clear + hide cursor */
    fflush(stdout);
}
static void neon_leave_screen(void) {
    printf("\033[?25h\033[0m"); fflush(stdout);
}

static void fmt_mmss(double sec, char *buf, size_t n) {
    _snprintf_s(buf, n, _TRUNCATE, "%02d:%02d", (int)(sec / 60), ((int)sec) % 60);
}

/* Full frame repaint. */
static void repaint(double t0, long long done, double now_cps, int avx512) {
    double el = neon_now() - t0;
    double avg = el > 0 ? (double)done / el : 0.0;
    double eta = avg > 0 ? (double)(NEON_RUN_TARGET - done) / avg : 0.0;
    char mmss[16], cbuf[32];

    printf("\033[H");
    box_top();
    lclr(); lput(" ");
    lcol(C_BOLD C_WHITE, "\xe2\x9a\xa1 1 BILLION CHUNKS \xc2\xb7 1:128 ONLY \xe2\x9a\xa1");
    flush_box();
    box_mid();

    /* stats row 1 */
    lclr(); lput(" ");
    lcol(C_WHITE, "Threads: "); lcol(C_BOLD C_GREEN, "10");
    lput("   ");
    lcol(C_WHITE, "Sampling: "); lcol(C_BOLD C_GREEN, "1:128 ");
    if (avx512) lcol(C_GREEN, "(AVX-512)");
    else        lcol(C_BOLD C_YELL, "(Only AVX2)");
    flush_box();

    /* stats row 2 */
    fmt_mmss(el, mmss, sizeof(mmss));
    neon_fmt(done, cbuf, sizeof(cbuf));
    lclr(); lput(" ");
    lcol(C_WHITE, "Elapsed: "); lcol(C_BOLD C_GREEN, mmss);
    lput("   ");
    lcol(C_WHITE, "Chunks: "); lcol(C_BOLD C_GREEN, cbuf);
    lcol(C_WHITE, " / 1,000,000,000");
    flush_box();
    box_mid();

    /* progress bar */
    {
        const int PW = 40;
        double frac = (double)done / (double)NEON_RUN_TARGET;
        if (frac < 0) frac = 0; if (frac > 1) frac = 1;
        int fill = (int)(frac * PW + 0.5);
        lclr(); lput("  ");
        lput(C_GREEN);
        for (int i = 0; i < fill; i++) lput("\xe2\x96\x88");          /* █ */
        lput(C_GRAY);
        for (int i = fill; i < PW; i++) lput("\xe2\x96\x91");          /* ░ */
        lput(C_RESET);
        _snprintf_s(cbuf, sizeof(cbuf), _TRUNCATE, " %5.1f%%", frac * 100.0);
        lcol(C_BOLD C_GREEN, cbuf);
        flush_box();
    }

    /* graph header */
    {
        double peak = 0;
        for (int i = 0; i < g_n; i++) if (g_samples[i] > peak) peak = g_samples[i];
        lclr(); lput("  ");
        lcol(C_WHITE, "THROUGHPUT (ch/s)");
        lput("   ");
        lcol(C_GRAY, "peak ");
        neon_fmt((long long)peak, cbuf, sizeof(cbuf)); lcol(C_YELL, cbuf);
        lcol(C_GRAY, "  now ");
        neon_fmt((long long)now_cps, cbuf, sizeof(cbuf)); lcol(C_BOLD C_GREEN, cbuf);
        flush_box();
    }

    /* Y window: phase 1 = 0-anchored warmup, phase 2 = detail zoom */
    double lo, hi, mx = 0, mn = 1e18;
    for (int i = 0; i < g_n; i++) { if (g_samples[i] > mx) mx = g_samples[i]; if (g_samples[i] < mn) mn = g_samples[i]; }
    if (g_n < 2) { lo = 0; hi = 1; }
    else if (el < NEON_WARMUP_SEC) { lo = 0; hi = mx * 1.05 + 1; }
    else {
        lo = mn * 0.95; hi = mx * 1.05;
        if (hi - lo < NEON_MIN_SPAN) {
            double mid = (hi + lo) * 0.5;
            lo = mid - NEON_MIN_SPAN * 0.5; hi = mid + NEON_MIN_SPAN * 0.5;
        }
    }
    render_graph(g_samples, g_n, lo, hi);
    draw_graph(lo, hi);

    /* x-axis */
    lclr(); lput("            ");
    lcol(C_GRAY, "\xe2\x94\x94");
    for (int i = 0; i < GR_W - 2; i++) lput(G_HLINE);
    lcol(C_GRAY, "\xe2\x94\x98");
    flush_box();
    box_bot();

    /* status line */
    {
        double peak = 0;
        for (int i = 0; i < g_n; i++) if (g_samples[i] > peak) peak = g_samples[i];
        neon_fmt((long long)peak, cbuf, sizeof(cbuf));
        fmt_mmss(eta, mmss, sizeof(mmss));
        lclr(); lput("  ");
        lcol(C_GRAY, "ETA "); lcol(C_YELL, mmss);
        lcol(C_GRAY, " \xc2\xb7 peak "); lcol(C_YELL, cbuf);
        lcol(C_GRAY, " \xc2\xb7 avg "); neon_fmt((long long)avg, cbuf, sizeof(cbuf)); lcol(C_WHITE, cbuf);
        lcol(C_GRAY, " \xc2\xb7 ");
        lcol(C_BOLD C_GREEN, "\xe2\x97\x8f RUNNING");
        flush_plain();
    }
    fflush(stdout);
}

int main(int argc, char **argv) {
    long long seed = 12345LL;
    if (argc > 1) {
        long long v = 0;
        if (sscanf(argv[1], "%lld", &v) == 1 && v >= 100000LL) {
            NEON_RUN_TARGET = v;
        } else {
            printf("Usage: Billion_Neon_Bench.exe [chunks]  (default 1,000,000,000, 1:128 only)\n");
            return 1;
        }
    }

    int avx512 = om_cpu_has_avx512();
    Generator *g = get_cached_generator((uint64_t)seed);
    if (!g) { fprintf(stderr, "generator init failed\n"); return 1; }
    g_correction_amp = 4;

    neon_enter_alt_screen();
    repaint(neon_now(), 0, 0, avx512);      /* draw the empty frame first */

    static int outs[NEON_WORKERS][256 * 3];
    HANDLE handles[NEON_WORKERS];
    neon_slice slices[NEON_WORKERS];
    volatile long long done = 0;
    long long per = NEON_RUN_TARGET / NEON_WORKERS;

    double t0 = neon_now();
    for (int t = 0; t < NEON_WORKERS; t++) {
        slices[t].seed = seed;
        slices[t].step = NEON_STEP;
        slices[t].from = (long long)t * per;
        slices[t].to   = (t == NEON_WORKERS - 1) ? NEON_RUN_TARGET
                                                 : (long long)(t + 1) * per;
        slices[t].core = t;
        slices[t].out  = outs[t];
        slices[t].done = &done;
        handles[t] = CreateThread(NULL, 0, neon_thread, &slices[t], 0, NULL);
        if (!handles[t]) {
            neon_leave_screen();
            fprintf(stderr, "thread creation failed\n");
            return 1;
        }
    }

    /* Monitor: sample + repaint every 250 ms until all workers finish. */
    double last_repaint = t0, last_sample = t0;
    long long last_done = 0;
    double ema = 0;
    for (;;) {
        DWORD w = WaitForMultipleObjects(NEON_WORKERS, handles, TRUE, 50);
        double now = neon_now();
        if (w == WAIT_OBJECT_0) break;
        if (now - last_repaint >= NEON_REPAINT_SEC) {
            long long d = done;
            double dt = now - last_sample;
            double inst = dt > 0 ? (double)(d - last_done) / dt : 0.0;
            ema = ema > 0 ? ema * 0.5 + inst * 0.5 : inst;
            last_sample = now; last_done = d;
            push_sample((float)ema);
            last_repaint = now;
            repaint(t0, d, ema, avx512);
        }
    }
    double sec = neon_now() - t0;
    for (int t = 0; t < NEON_WORKERS; t++) CloseHandle(handles[t]);

    double cps = (double)NEON_RUN_TARGET / sec;

    /* ── Finale ── */
    printf("\033[2J\033[H");
    box_top();
    lclr(); lput(" ");
    lcol(C_BOLD C_WHITE, "\xe2\x9a\xa1 1 BILLION CHUNKS \xc2\xb7 RESULT \xe2\x9a\xa1");
    flush_box();
    box_mid();
    {
        char cbuf[32], mmss[16];
        neon_fmt((long long)cps, cbuf, sizeof(cbuf));
        lclr(); lput("   ");
        lcol(C_WHITE, "1:128  ->  ");
        lcol(C_BOLD C_GREEN, cbuf);
        lcol(C_WHITE, " chunks/sec");
        flush_box();

        neon_fmt(NEON_RUN_TARGET, cbuf, sizeof(cbuf));
        lclr(); lput("   ");
        lcol(C_WHITE, "Chunks: ");  lcol(C_BOLD C_GREEN, cbuf);
        fmt_mmss(sec, mmss, sizeof(mmss));
        lcol(C_WHITE, "   Time: "); lcol(C_YELL, mmss);
        flush_box();

        double peak = cps, mn = cps;
        for (int i = 0; i < g_hist_n; i++) { if (g_hist[i] > peak) peak = g_hist[i]; if (g_hist[i] < mn) mn = g_hist[i]; }
        lclr(); lput("   ");
        lcol(C_GRAY, "peak "); neon_fmt((long long)peak, cbuf, sizeof(cbuf)); lcol(C_YELL, cbuf);
        lcol(C_GRAY, " \xc2\xb7 steady-low "); neon_fmt((long long)mn, cbuf, sizeof(cbuf)); lcol(C_WHITE, cbuf);
        flush_box();
    }
    box_mid();
    /* Short runs (<~2 s): live history never filled — copy the live ring. */
    if (g_hist_n < 2 && g_n > 0) {
        for (int i = 0; i < g_n; i++) g_hist[i] = g_samples[i];
        g_hist_n = g_n;
    }
    {   /* full-run sparkline (0-anchored so the warmup ramp shows) */
        double hi = 1;
        for (int i = 0; i < g_hist_n; i++) if (g_hist[i] > hi) hi = g_hist[i];
        hi *= 1.05;
        lclr(); lput("  ");
        lcol(C_GRAY, "FULL RUN (ch/s)"); flush_box();
        render_graph(g_hist, g_hist_n, 0, hi);
        draw_graph(0, hi);
        lclr(); lput("            ");
        lcol(C_GRAY, "\xe2\x94\x94");
        for (int i = 0; i < GR_W - 2; i++) lput(G_HLINE);
        lcol(C_GRAY, "\xe2\x94\x98");
        flush_box();
    }
    box_bot();
    lclr(); lput("  ");
    lcol(C_BOLD C_GREEN, "\xe2\x97\x8f DONE");
    lcol(C_GRAY, " \xe2\x80\x94 ");
    lcol(C_WHITE, "Press Enter to exit...");
    flush_plain();
    fflush(stdout);

    neon_leave_screen();
    getchar();
    return 0;
}
