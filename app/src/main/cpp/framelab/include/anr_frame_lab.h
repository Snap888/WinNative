#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ANR Frame Lab capture stages. Pixel payloads are lossless and stored verbatim. */
enum anr_fl_stage {
    ANR_FL_STAGE_SOURCE = 1,
    ANR_FL_STAGE_PRE_EFFECT = 2,
    ANR_FL_STAGE_EFFECT_OUTPUT = 3,
    ANR_FL_STAGE_FRAMEGEN_INPUT = 4,
    ANR_FL_STAGE_FRAMEGEN_OUTPUT = 5,
    ANR_FL_STAGE_FINAL_OUTPUT = 6
};

typedef struct anr_fl_stats {
    uint64_t submitted;
    uint64_t written;
    uint64_t dropped;
    uint64_t bytes_written;
    int last_error;
} anr_fl_stats;

/* directory must exist and be writable. Creates record.json, frames.idx,
 * frames.bin and params.bin. One writer thread drains a bounded 3-slot queue. */
int anr_fl_start(const char *directory);
int anr_fl_submit(uint32_t stage, uint64_t frame_id, uint64_t timestamp_ns,
                  uint32_t width, uint32_t height, uint32_t format,
                  const void *pixels, size_t pixel_bytes,
                  const void *params, size_t params_bytes);
void anr_fl_get_stats(anr_fl_stats *out_stats);
int anr_fl_stop(void);

#ifdef __cplusplus
}
#endif
