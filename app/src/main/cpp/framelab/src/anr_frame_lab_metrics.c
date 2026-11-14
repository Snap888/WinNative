#include "anr_frame_lab_metrics.h"
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FL_MAX_PIXELS ((uint64_t)16384u * 16384u)
#define FL_SSIM_WINDOW 8u
#define FL_MOTION_RADIUS 8
#define FL_MOTION_SAMPLE_STEP 4u

static double ssim_window(const uint8_t *a, const uint8_t *b, uint32_t w,
                          uint32_t x0, uint32_t y0, uint32_t ww, uint32_t hh) {
    const uint64_t n = (uint64_t)ww * hh * 3u;
    long double ma = 0.0L, mb = 0.0L;
    for (uint32_t y = 0; y < hh; ++y) {
        const uint64_t row = (uint64_t)(y0 + y) * w + x0;
        for (uint32_t x = 0; x < ww; ++x) {
            const uint64_t p = (row + x) * 4u;
            for (unsigned c = 0; c < 3; ++c) {
                ma += a[p + c];
                mb += b[p + c];
            }
        }
    }
    ma /= (long double)n;
    mb /= (long double)n;
    long double va = 0.0L, vb = 0.0L, cov = 0.0L;
    for (uint32_t y = 0; y < hh; ++y) {
        const uint64_t row = (uint64_t)(y0 + y) * w + x0;
        for (uint32_t x = 0; x < ww; ++x) {
            const uint64_t p = (row + x) * 4u;
            for (unsigned c = 0; c < 3; ++c) {
                const long double da = (long double)a[p + c] - ma;
                const long double db = (long double)b[p + c] - mb;
                va += da * da;
                vb += db * db;
                cov += da * db;
            }
        }
    }
    va /= (long double)n;
    vb /= (long double)n;
    cov /= (long double)n;
    const long double c1 = 6.5025L, c2 = 58.5225L;
    const long double den = (ma * ma + mb * mb + c1) * (va + vb + c2);
    return den > 0.0L
        ? (double)(((2.0L * ma * mb + c1) * (2.0L * cov + c2)) / den)
        : 1.0;
}

int anr_fl_compare_rgba8(const uint8_t *a, const uint8_t *b,
                         uint32_t w, uint32_t h, anr_fl_metrics *out) {
    if (!a || !b || !out || !w || !h || (uint64_t)w * h > FL_MAX_PIXELS) return EINVAL;
    const uint64_t pixels = (uint64_t)w * h;
    const uint64_t n = pixels * 3u;
    long double squared = 0.0L, absolute = 0.0L;
    for (uint64_t p = 0; p < pixels; ++p) {
        for (unsigned c = 0; c < 3; ++c) {
            const long double d = (long double)a[p * 4u + c] - b[p * 4u + c];
            squared += d * d;
            absolute += fabsl(d);
        }
    }

    long double ssim_sum = 0.0L;
    uint64_t windows = 0;
    for (uint32_t y = 0; y < h; y += FL_SSIM_WINDOW) {
        const uint32_t hh = h - y < FL_SSIM_WINDOW ? h - y : FL_SSIM_WINDOW;
        for (uint32_t x = 0; x < w; x += FL_SSIM_WINDOW) {
            const uint32_t ww = w - x < FL_SSIM_WINDOW ? w - x : FL_SSIM_WINDOW;
            ssim_sum += ssim_window(a, b, w, x, y, ww, hh);
            ++windows;
        }
    }

    const long double mse = squared / (long double)n;
    out->psnr_db = mse <= 0.0L ? INFINITY : (double)(10.0L * log10l(65025.0L / mse));
    out->ssim = windows ? (double)(ssim_sum / (long double)windows) : 1.0;
    out->mean_absolute_error = (double)(absolute / (long double)n);
    out->compared_samples = n;
    out->temporal_flicker = 0.0;
    out->temporal_stability = 1.0;
    out->motion_x = 0;
    out->motion_y = 0;
    return 0;
}

/* Estimate a single global integer-pixel translation. This is intentionally conservative:
 * a small coarse SAD search removes camera/pan motion without pretending to be dense optical flow. */
static void estimate_translation(const uint8_t *prev, const uint8_t *cur,
                                 uint32_t w, uint32_t h, int *best_dx, int *best_dy) {
    long double best = HUGE_VALL;
    *best_dx = *best_dy = 0;
    const int radius_x = w < (uint32_t)(2 * FL_MOTION_RADIUS + 1)
        ? (int)(w - 1u) : FL_MOTION_RADIUS;
    const int radius_y = h < (uint32_t)(2 * FL_MOTION_RADIUS + 1)
        ? (int)(h - 1u) : FL_MOTION_RADIUS;
    for (int dy = -radius_y; dy <= radius_y; ++dy) {
        for (int dx = -radius_x; dx <= radius_x; ++dx) {
            long double sad = 0.0L;
            uint64_t count = 0;
            const uint32_t y0 = dy < 0 ? (uint32_t)-dy : 0u;
            const uint32_t x0 = dx < 0 ? (uint32_t)-dx : 0u;
            const uint32_t y1 = dy > 0 ? h - (uint32_t)dy : h;
            const uint32_t x1 = dx > 0 ? w - (uint32_t)dx : w;
            for (uint32_t y = y0; y < y1; y += FL_MOTION_SAMPLE_STEP) {
                const uint32_t py = (uint32_t)((int)y - dy);
                for (uint32_t x = x0; x < x1; x += FL_MOTION_SAMPLE_STEP) {
                    const uint32_t px = (uint32_t)((int)x - dx);
                    const uint64_t p = ((uint64_t)y * w + x) * 4u;
                    const uint64_t q = ((uint64_t)py * w + px) * 4u;
                    const int ga = (77 * cur[p] + 150 * cur[p + 1] + 29 * cur[p + 2]) >> 8;
                    const int gb = (77 * prev[q] + 150 * prev[q + 1] + 29 * prev[q + 2]) >> 8;
                    sad += (long double)abs(ga - gb);
                    ++count;
                }
            }
            if (count && sad / (long double)count < best) {
                best = sad / (long double)count;
                *best_dx = dx;
                *best_dy = dy;
            }
        }
    }
}

int anr_fl_temporal_rgba8(const uint8_t *previous, const uint8_t *current,
                          uint32_t w, uint32_t h, anr_fl_metrics *m) {
    if (!previous || !current || !m || !w || !h ||
        (uint64_t)w * h > FL_MAX_PIXELS) return EINVAL;
    int dx, dy;
    estimate_translation(previous, current, w, h, &dx, &dy);
    const uint32_t x0 = dx > 0 ? (uint32_t)dx : 0u;
    const uint32_t y0 = dy > 0 ? (uint32_t)dy : 0u;
    const uint32_t x1 = dx < 0 ? w - (uint32_t)-dx : w;
    const uint32_t y1 = dy < 0 ? h - (uint32_t)-dy : h;
    long double delta = 0.0L;
    uint64_t count = 0;
    for (uint32_t y = y0; y < y1; ++y) {
        const uint32_t py = (uint32_t)((int)y - dy);
        for (uint32_t x = x0; x < x1; ++x) {
            const uint32_t px = (uint32_t)((int)x - dx);
            const uint64_t p = ((uint64_t)y * w + x) * 4u;
            const uint64_t q = ((uint64_t)py * w + px) * 4u;
            for (unsigned c = 0; c < 3; ++c) {
                delta += fabsl((long double)current[p + c] - previous[q + c]);
                ++count;
            }
        }
    }
    const double flicker = count ? (double)(delta / (long double)count) : 0.0;
    m->temporal_flicker = flicker;
    m->temporal_stability = 1.0 - flicker / 255.0;
    if (m->temporal_stability < 0.0) m->temporal_stability = 0.0;
    m->motion_x = dx;
    m->motion_y = dy;
    return 0;
}
