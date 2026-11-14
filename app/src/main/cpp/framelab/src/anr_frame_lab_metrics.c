#include "anr_frame_lab_metrics.h"
#include <errno.h>
#include <math.h>
#include <string.h>

#define FL_MAX_PIXELS ((uint64_t)16384u * 16384u)

int anr_fl_compare_rgba8(const uint8_t *a, const uint8_t *b,
                         uint32_t w, uint32_t h, anr_fl_metrics *out) {
    if (!a || !b || !out || !w || !h || (uint64_t)w * h > FL_MAX_PIXELS) return EINVAL;
    const uint64_t pixels = (uint64_t)w * h;
    const uint64_t n = pixels * 3u;
    long double squared = 0.0L, absolute = 0.0L;
    long double ma = 0.0L, mb = 0.0L, va = 0.0L, vb = 0.0L, cov = 0.0L;
    for (uint64_t p = 0; p < pixels; ++p) {
        for (unsigned c = 0; c < 3; ++c) {
            const long double x = a[p * 4u + c], y = b[p * 4u + c];
            const long double d = x - y;
            squared += d * d;
            absolute += fabsl(d);
            ma += x; mb += y;
        }
    }
    ma /= (long double)n; mb /= (long double)n;
    for (uint64_t p = 0; p < pixels; ++p) {
        for (unsigned c = 0; c < 3; ++c) {
            const long double x = a[p * 4u + c] - ma;
            const long double y = b[p * 4u + c] - mb;
            va += x * x; vb += y * y; cov += x * y;
        }
    }
    va /= (long double)n; vb /= (long double)n; cov /= (long double)n;
    const long double mse = squared / (long double)n;
    const long double c1 = 6.5025L, c2 = 58.5225L;
    const long double den = (ma * ma + mb * mb + c1) * (va + vb + c2);
    out->psnr_db = mse <= 0.0L ? INFINITY : (double)(10.0L * log10l(65025.0L / mse));
    out->ssim = den > 0.0L
        ? (double)(((2.0L * ma * mb + c1) * (2.0L * cov + c2)) / den) : 1.0;
    out->mean_absolute_error = (double)(absolute / (long double)n);
    out->compared_samples = n;
    out->temporal_flicker = 0.0;
    out->temporal_stability = 1.0;
    return 0;
}

int anr_fl_temporal_rgba8(const uint8_t *previous, const uint8_t *current,
                          uint32_t w, uint32_t h, anr_fl_metrics *m) {
    if (!previous || !current || !m || !w || !h ||
        (uint64_t)w * h > FL_MAX_PIXELS) return EINVAL;
    const uint64_t n = (uint64_t)w * h * 3u;
    long double delta = 0.0L;
    for (uint64_t p = 0; p < (uint64_t)w * h; ++p)
        for (unsigned c = 0; c < 3; ++c)
            delta += fabsl((long double)current[p * 4u + c] -
                           (long double)previous[p * 4u + c]);
    const double flicker = (double)(delta / (long double)n);
    m->temporal_flicker = flicker;
    m->temporal_stability = 1.0 - flicker / 255.0;
    if (m->temporal_stability < 0.0) m->temporal_stability = 0.0;
    return 0;
}
