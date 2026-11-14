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


#define ANR_FL_CONFIG_VERSION 1u
#define ANR_FL_CONFIG_MAX_EFFECTS 8u
enum anr_fl_config_flags {
    ANR_FL_CFG_SNAPFRAME = 1u << 0,
    ANR_FL_CFG_WFG       = 1u << 1,
    ANR_FL_CFG_LSFG      = 1u << 2
};
typedef struct anr_fl_effect_config {
    uint32_t effect_id;
    uint32_t mode;
    float params[4];
} anr_fl_effect_config;
/* Stable per-frame settings snapshot stored verbatim in params.bin. */
typedef struct anr_fl_config_v1 {
    uint32_t struct_size, version, feature_flags, engine;
    uint32_t snapframe_mode, wfg_mode, lsfg_mode, multiplier;
    uint32_t target_fps, dis_min_side, effect_count, reserved;
    float flow_scale;
    anr_fl_effect_config effects[ANR_FL_CONFIG_MAX_EFFECTS];
} anr_fl_config_v1;

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
int anr_fl_submit_configured(uint32_t stage, uint64_t frame_id, uint64_t timestamp_ns,\n                             uint32_t width, uint32_t height, uint32_t format,\n                             const void *pixels, size_t pixel_bytes,\n                             const anr_fl_config_v1 *config);\nvoid anr_fl_get_stats(anr_fl_stats *out_stats);
int anr_fl_stop(void);

#ifdef __cplusplus
}
#endif
