#ifndef SNAP_AI_ENGINE_H
#define SNAP_AI_ENGINE_H
#include <stdint.h>

#if defined(_WIN32)
  #if defined(SNAP_AI_ENGINE_BUILD)
    #define SNAP_AI_API __declspec(dllexport)
  #else
    #define SNAP_AI_API __declspec(dllimport)
  #endif
#else
  #define SNAP_AI_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SNAP_AI_API_VERSION 1u
typedef struct SnapAIHandle SnapAIHandle;

typedef enum SnapAIProfile {
  SNAP_AI_PROFILE_PERFORMANCE = 0,
  SNAP_AI_PROFILE_BALANCED = 1,
  SNAP_AI_PROFILE_QUALITY = 2
} SnapAIProfile;

typedef enum SnapAIMode {
  SNAP_AI_MODE_BYPASS = 0,
  SNAP_AI_MODE_CONSERVATIVE = 1,
  SNAP_AI_MODE_ADAPTIVE = 2,
  SNAP_AI_MODE_QUALITY = 3
} SnapAIMode;

typedef enum SnapAIStatus {
  SNAP_AI_OK = 0,
  SNAP_AI_ERROR_INVALID_ARGUMENT = -1,
  SNAP_AI_ERROR_VERSION_MISMATCH = -2,
  SNAP_AI_ERROR_INTERNAL = -3
} SnapAIStatus;

typedef struct SnapAIConfig {
  uint32_t struct_size;
  uint32_t api_version;
  SnapAIProfile profile;
  uint32_t enabled;
  float target_gpu_budget_ms;
  float min_flow_scale;
  float max_flow_scale;
  uint32_t min_refinement_passes;
  uint32_t max_refinement_passes;
  float hysteresis;
  uint32_t warmup_frames;
} SnapAIConfig;

typedef struct SnapAIFrameMetrics {
  uint32_t struct_size;
  uint64_t frame_id;
  float frame_time_ms;
  float gpu_time_ms;
  float motion_magnitude;
  float motion_complexity;
  float flow_confidence;
  float gpu_pressure;
  float artifact_risk;
  uint32_t scene_cut;
  uint32_t telemetry_valid;
} SnapAIFrameMetrics;

typedef struct SnapAIDecision {
  uint32_t struct_size;
  uint32_t api_version;
  SnapAIMode mode;
  float flow_scale;
  float search_radius_scale;
  float confidence_threshold;
  uint32_t refinement_passes;
  uint32_t reuse_previous_flow;
  uint32_t run_bidirectional_flow;
  uint32_t run_artifact_check;
  float estimated_compute_saving;
  uint64_t frame_id;
} SnapAIDecision;

typedef struct SnapAIStats {
  uint32_t struct_size;
  uint64_t frames_seen;
  uint64_t bypass_frames;
  uint64_t adaptive_frames;
  float average_motion_complexity;
  float average_flow_confidence;
  float average_gpu_pressure;
  float last_flow_scale;
} SnapAIStats;

SNAP_AI_API void snap_ai_default_config(SnapAIConfig* config);
SNAP_AI_API SnapAIHandle* snap_ai_create(const SnapAIConfig* config);
SNAP_AI_API void snap_ai_destroy(SnapAIHandle* handle);
SNAP_AI_API SnapAIStatus snap_ai_update(SnapAIHandle* handle,
  const SnapAIFrameMetrics* metrics, SnapAIDecision* decision);
SNAP_AI_API SnapAIStatus snap_ai_set_profile(SnapAIHandle* handle, SnapAIProfile profile);
SNAP_AI_API SnapAIStatus snap_ai_set_enabled(SnapAIHandle* handle, uint32_t enabled);
SNAP_AI_API SnapAIStatus snap_ai_get_stats(SnapAIHandle* handle, SnapAIStats* stats);
SNAP_AI_API uint32_t snap_ai_get_api_version(void);

#ifdef __cplusplus
}
#endif
#endif
