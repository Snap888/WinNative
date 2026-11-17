#include "snap_frame/snap_frame.h"
#include <atomic>
#include <cmath>
#include <cstring>
#include <new>
#include <mutex>

struct SnapFrameEngine {
  SnapFrameEngineConfig config{};
  SnapFrameBackendV1 backend{};
  std::mutex config_mutex;
  std::atomic_flag processing = ATOMIC_FLAG_INIT;
  std::atomic<uint64_t> frames_seen{0}, frames_processed{0}, generated_frames{0};
  std::atomic<uint64_t> bypassed_frames{0}, failed_frames{0}, last_frame_id{0};
  std::atomic<float> last_total_gpu_ms{0.0f};
  bool initialized = false;
};

namespace {
bool validConfig(const SnapFrameEngineConfig* c) {
  return c && c->struct_size >= sizeof(SnapFrameEngineConfig)
    && c->api_version == SNAP_FRAME_API_VERSION
    && c->multiplier >= 1 && c->multiplier <= SNAP_FRAME_MAX_MULTIPLIER
    && c->quality >= SNAP_FRAME_QUALITY_PERFORMANCE
    && c->quality <= SNAP_FRAME_QUALITY_QUALITY
    && c->flow_scale_percent >= 25 && c->flow_scale_percent <= 100
    && (!c->diagnostic_suite || (c->diagnostic_phase_frames >= 300
      && c->diagnostic_phase_frames <= 36000));
}
bool validBackend(const SnapFrameBackendV1* b) {
  return b && b->struct_size >= sizeof(SnapFrameBackendV1)
    && b->api_version == SNAP_FRAME_API_VERSION && b->process_frame;
}
float totalGpu(const SnapFrameFrameOutput& o) {
  return o.capture_gpu_ms + o.preprocess_gpu_ms + o.flow_gpu_ms
    + o.consistency_gpu_ms + o.warp_gpu_ms + o.composite_gpu_ms;
}
}

extern "C" void snap_frame_default_config(SnapFrameEngineConfig* c) {
  if (!c) return;
  std::memset(c, 0, sizeof(*c));
  c->struct_size = sizeof(*c);
  c->api_version = SNAP_FRAME_API_VERSION;
  c->enabled = 0;
  c->multiplier = 2;
  c->quality = SNAP_FRAME_QUALITY_BALANCED;
  c->flow_scale_percent = 75;
  c->motion_analysis = 1;
  c->profiling_enabled = 1;
  c->diagnostic_phase_frames = 1800;
}

extern "C" SnapFrameEngine* snap_frame_create(
    const SnapFrameEngineConfig* config, const SnapFrameBackendV1* backend) {
  SnapFrameEngineConfig c{};
  if (config) c = *config; else snap_frame_default_config(&c);
  if (!validConfig(&c) || !validBackend(backend)) return nullptr;
  auto* e = new (std::nothrow) SnapFrameEngine();
  if (!e) return nullptr;
  e->config = c;
  e->backend = *backend;
  if (e->backend.initialize) {
    const auto status = e->backend.initialize(e->backend.user_data, &e->config);
    if (status != SNAP_FRAME_OK) { delete e; return nullptr; }
  }
  e->initialized = true;
  return e;
}

extern "C" void snap_frame_destroy(SnapFrameEngine* e) {
  if (!e) return;
  while (e->processing.test_and_set(std::memory_order_acquire)) {}
  if (e->initialized && e->backend.shutdown)
    e->backend.shutdown(e->backend.user_data);
  e->initialized = false;
  e->processing.clear(std::memory_order_release);
  delete e;
}

extern "C" SnapFrameStatus snap_frame_set_config(
    SnapFrameEngine* e, const SnapFrameEngineConfig* c) {
  if (!e || !validConfig(c)) return SNAP_FRAME_ERROR_INVALID_ARGUMENT;
  /* Serialize config changes with process_frame so backends may safely update
   * render-thread-owned state without racing GPU command recording. */
  if (e->processing.test_and_set(std::memory_order_acquire))
    return SNAP_FRAME_ERROR_BUSY;
  struct ProcessingGuard {
    std::atomic_flag& flag;
    ~ProcessingGuard() { flag.clear(std::memory_order_release); }
  } guard{e->processing};
  std::lock_guard<std::mutex> lock(e->config_mutex);
  if (e->backend.update_config) {
    const auto status = e->backend.update_config(e->backend.user_data, c);
    if (status != SNAP_FRAME_OK) return status;
  } else if (e->config.multiplier != c->multiplier
      || e->config.quality != c->quality
      || e->config.flow_scale_percent != c->flow_scale_percent
      || e->config.motion_analysis != c->motion_analysis) {
    /* Without update_config, only enable/profiling/diagnostic toggles are hot. */
    return SNAP_FRAME_ERROR_BACKEND;
  }
  const bool wasEnabled = e->config.enabled != 0;
  e->config = *c;
  if (wasEnabled != (e->config.enabled != 0) && e->backend.reset_history)
    e->backend.reset_history(e->backend.user_data, 1u);
  return SNAP_FRAME_OK;
}

extern "C" SnapFrameStatus snap_frame_set_enabled(SnapFrameEngine* e, uint32_t enabled) {
  if (!e) return SNAP_FRAME_ERROR_INVALID_ARGUMENT;
  SnapFrameEngineConfig c;
  { std::lock_guard<std::mutex> lock(e->config_mutex); c = e->config; }
  c.enabled = enabled ? 1u : 0u;
  return snap_frame_set_config(e, &c);
}

extern "C" SnapFrameStatus snap_frame_set_quality(SnapFrameEngine* e, SnapFrameEngineQuality q) {
  if (!e || q < SNAP_FRAME_QUALITY_PERFORMANCE || q > SNAP_FRAME_QUALITY_QUALITY)
    return SNAP_FRAME_ERROR_INVALID_ARGUMENT;
  SnapFrameEngineConfig c;
  { std::lock_guard<std::mutex> lock(e->config_mutex); c = e->config; }
  c.quality = q;
  return snap_frame_set_config(e, &c);
}

extern "C" SnapFrameStatus snap_frame_process(SnapFrameEngine* e,
    const SnapFrameFrameInput* in, SnapFrameFrameOutput* out) {
  if (!e || !in || !out || in->struct_size < sizeof(*in)
      || out->struct_size < sizeof(*out) || !in->frame_id || !in->width || !in->height)
    return SNAP_FRAME_ERROR_INVALID_ARGUMENT;
  e->frames_seen.fetch_add(1, std::memory_order_relaxed);
  if (e->processing.test_and_set(std::memory_order_acquire))
    return SNAP_FRAME_ERROR_BUSY;
  SnapFrameEngineConfig c;
  { std::lock_guard<std::mutex> lock(e->config_mutex); c = e->config; }
  std::memset(reinterpret_cast<char*>(out) + sizeof(uint32_t), 0,
      sizeof(*out) - sizeof(uint32_t));
  if (!c.enabled || c.multiplier == 1) {
    e->bypassed_frames.fetch_add(1, std::memory_order_relaxed);
    e->last_frame_id.store(in->frame_id, std::memory_order_relaxed);
    if (in->scene_cut && e->backend.reset_history)
      e->backend.reset_history(e->backend.user_data, 2u);
    e->processing.clear(std::memory_order_release);
    return SNAP_FRAME_BYPASSED;
  }
  const uint64_t previous = e->last_frame_id.load(std::memory_order_relaxed);
  if (previous && in->frame_id <= previous) {
    e->failed_frames.fetch_add(1, std::memory_order_relaxed);
    e->processing.clear(std::memory_order_release);
    return SNAP_FRAME_ERROR_FRAME_ORDER;
  }
  if (in->scene_cut && e->backend.reset_history)
    e->backend.reset_history(e->backend.user_data, 2u);
  out->struct_size = sizeof(*out);
  const auto status = e->backend.process_frame(e->backend.user_data, in, out);
  e->last_frame_id.store(in->frame_id, std::memory_order_relaxed);
  const bool validTimings = std::isfinite(out->capture_gpu_ms) && out->capture_gpu_ms >= 0.0f
    && std::isfinite(out->preprocess_gpu_ms) && out->preprocess_gpu_ms >= 0.0f
    && std::isfinite(out->flow_gpu_ms) && out->flow_gpu_ms >= 0.0f
    && std::isfinite(out->consistency_gpu_ms) && out->consistency_gpu_ms >= 0.0f
    && std::isfinite(out->warp_gpu_ms) && out->warp_gpu_ms >= 0.0f
    && std::isfinite(out->composite_gpu_ms) && out->composite_gpu_ms >= 0.0f;
  if (status == SNAP_FRAME_OK && validTimings && out->generated_frames <= c.multiplier - 1u) {
    e->frames_processed.fetch_add(1, std::memory_order_relaxed);
    e->generated_frames.fetch_add(out->generated_frames, std::memory_order_relaxed);
    e->last_total_gpu_ms.store(totalGpu(*out), std::memory_order_relaxed);
  } else {
    e->failed_frames.fetch_add(1, std::memory_order_relaxed);
    if (status == SNAP_FRAME_OK) {
      e->processing.clear(std::memory_order_release);
      return SNAP_FRAME_ERROR_BACKEND;
    }
  }
  e->processing.clear(std::memory_order_release);
  return status;
}

extern "C" SnapFrameStatus snap_frame_get_stats(SnapFrameEngine* e, SnapFrameEngineStats* s) {
  if (!e || !s || s->struct_size < sizeof(*s))
    return SNAP_FRAME_ERROR_INVALID_ARGUMENT;
  s->frames_seen = e->frames_seen.load(std::memory_order_relaxed);
  s->frames_processed = e->frames_processed.load(std::memory_order_relaxed);
  s->generated_frames = e->generated_frames.load(std::memory_order_relaxed);
  s->bypassed_frames = e->bypassed_frames.load(std::memory_order_relaxed);
  s->failed_frames = e->failed_frames.load(std::memory_order_relaxed);
  s->last_frame_id = e->last_frame_id.load(std::memory_order_relaxed);
  s->last_total_gpu_ms = e->last_total_gpu_ms.load(std::memory_order_relaxed);
  return SNAP_FRAME_OK;
}
extern "C" uint32_t snap_frame_get_api_version(void) { return SNAP_FRAME_API_VERSION; }
