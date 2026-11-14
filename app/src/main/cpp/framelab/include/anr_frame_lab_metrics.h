#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct anr_fl_metrics {
    double psnr_db;
    double ssim;
    double mean_absolute_error;
    double temporal_flicker;
    double temporal_stability;
    uint64_t compared_samples;
} anr_fl_metrics;

/* Compare two tightly packed RGBA8 frames. Alpha is ignored. */
int anr_fl_compare_rgba8(const uint8_t *reference, const uint8_t *candidate,
                         uint32_t width, uint32_t height, anr_fl_metrics *out);
/* Mean absolute per-channel change between consecutive RGBA8 frames (0..255). */
int anr_fl_temporal_rgba8(const uint8_t *previous, const uint8_t *current,
                          uint32_t width, uint32_t height, anr_fl_metrics *inout);
#ifdef __cplusplus
}
#endif
