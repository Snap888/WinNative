#ifndef SNAP_FRAME_ENGINE_H
#define SNAP_FRAME_ENGINE_H
#include <stdint.h>
#include <stddef.h>

#if defined(_WIN32)
  #if defined(SNAP_FRAME_ENGINE_BUILD)
    #define SNAP_FRAME_API __declspec(dllexport)
  #else
    #define SNAP_FRAME_API __declspec(dllimport)
  #endif
#else
  #define SNAP_FRAME_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SNAP_FRAME_API_VERSION 1u
#define SNAP_FRAME_MAX_MULTIPLIER 4u
typedef struct SnapFrameEngine SnapFrameEngine;

typedef enum SnapFrameStatus {
  SNAP_FRAME_OK = 0,
  SNAP_FRAME_BYPASSED = 1,
  SNAP_FRAME_ERROR_INVALID_ARGUMENT = -1,
  SNAP_FRAME_ERROR_VERSION_MISMATCH = -2,
  SNAP_FRAME_ERROR_BACKEND = -3,
  SNAP_FRAME_ERROR_BUSY = -4,
  SNAP_FRAME_ERROR_FRAME_ORDER = -5
} SnapFrameStatus;

typedef enum SnapFrameEngineQuality {
  SNAP_FRAME_QUALITY_PERFORMANCE = 0,
  SNAP_FRAME_QUALITY_BALANCED = 1,
  SNAP_FRAME_QUALITY_QUALITY = 2
} SnapFrameEngineQuality;

typedef struct SnapFrameEngineConfig {
  uint32_t struct_size;
  uint32_t api_version;
  uint32_t enabled;
  uint32_t multiplier;          /* v1: 1 (bypass) or 2..4 (up to 3 temporal phases). */
  SnapFrameEngineQuality quality;
  uint32_t flow_scale_percent;  /* 25..100 */
  uint32_t motion_analysis;
  uint32_t profiling_enabled;
  uint32_t diagnostic_suite;
  uint32_t diagnostic_phase_frames;
} SnapFrameEngineConfig;

/* Native handles are opaque integer tokens. The core never interprets them.
 * A backend may map them to Vulkan/D3D/Metal objects in its own adapter. */
typedef struct SnapFrameFrameInput {
  uint32_t struct_size;
  uint64_t frame_id;
  uint64_t timestamp_ns;
  uint32_t width;
  uint32_t height;
  uint32_t format;
  uint32_t scene_cut;
  uintptr_t native_frame;
} SnapFrameFrameInput;

typedef struct SnapFrameFrameOutput {
  uint32_t struct_size;
  uint32_t generated_frames;
  uint32_t flags;
  float capture_gpu_ms;
  float preprocess_gpu_ms;
  float flow_gpu_ms;
  float consistency_gpu_ms;
  float warp_gpu_ms;
  float composite_gpu_ms;
} SnapFrameFrameOutput;

typedef struct SnapFrameEngineStats {
  uint32_t struct_size;
  uint64_t frames_seen;
  uint64_t frames_processed;
  uint64_t generated_frames;
  uint64_t bypassed_frames;
  uint64_t failed_frames;
  uint64_t last_frame_id;
  float last_total_gpu_ms;
} SnapFrameEngineStats;

typedef struct SnapFrameBackendV1 {
  uint32_t struct_size;
  uint32_t api_version;
  void* user_data;
  /* Called once on create and on explicit hot configuration changes. */
  SnapFrameStatus (*initialize)(void* user_data, const SnapFrameEngineConfig* config);
  void (*shutdown)(void* user_data);
  /* Optional; must not destroy host-owned synchronization primitives. */
  SnapFrameStatus (*update_config)(void* user_data, const SnapFrameEngineConfig* config);
  void (*reset_history)(void* user_data, uint32_t reason);
  /* Records/submits backend work using host-owned resources and synchronization.
   * Must not wait for GPU completion or perform CPU readback in the frame path.
   * For multiplier M, emit at most M-1 frames in chronological temporal-phase order. */
  SnapFrameStatus (*process_frame)(void* user_data,
      const SnapFrameFrameInput* input, SnapFrameFrameOutput* output);
} SnapFrameBackendV1;

SNAP_FRAME_API void snap_frame_default_config(SnapFrameEngineConfig* config);
SNAP_FRAME_API SnapFrameEngine* snap_frame_create(
    const SnapFrameEngineConfig* config, const SnapFrameBackendV1* backend);
SNAP_FRAME_API void snap_frame_destroy(SnapFrameEngine* engine);
SNAP_FRAME_API SnapFrameStatus snap_frame_set_config(
    SnapFrameEngine* engine, const SnapFrameEngineConfig* config);
SNAP_FRAME_API SnapFrameStatus snap_frame_set_enabled(
    SnapFrameEngine* engine, uint32_t enabled);
SNAP_FRAME_API SnapFrameStatus snap_frame_set_quality(
    SnapFrameEngine* engine, SnapFrameEngineQuality quality);
SNAP_FRAME_API SnapFrameStatus snap_frame_process(
    SnapFrameEngine* engine, const SnapFrameFrameInput* input,
    SnapFrameFrameOutput* output);
SNAP_FRAME_API SnapFrameStatus snap_frame_get_stats(
    SnapFrameEngine* engine, SnapFrameEngineStats* stats);
SNAP_FRAME_API uint32_t snap_frame_get_api_version(void);

#ifdef __cplusplus
}
#endif
#endif
