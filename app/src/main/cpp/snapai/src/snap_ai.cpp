#include "snap_ai/snap_ai.h"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <new>

struct SnapAIHandle {
  SnapAIConfig cfg{};
  SnapAIStats stats{};
  std::mutex mutex;
  uint32_t warmup_left = 0;
  float complexity = 0.0f, confidence = 1.0f, pressure = 0.0f;
  float previous_scale = 1.0f;
  uint64_t last_frame_id = 0;
  bool have_frame = false;
};

namespace {
float clamp01(float x) { return std::max(0.0f, std::min(1.0f, x)); }
float clampf(float x, float lo, float hi) { return std::max(lo, std::min(hi, x)); }
bool finite(float x) { return std::isfinite(x); }
bool valid_profile(SnapAIProfile p) {
  return p == SNAP_AI_PROFILE_PERFORMANCE || p == SNAP_AI_PROFILE_BALANCED ||
         p == SNAP_AI_PROFILE_QUALITY;
}
void bypass(const SnapAIHandle* h, uint64_t id, SnapAIDecision* d) {
  d->struct_size = sizeof(*d); d->api_version = SNAP_AI_API_VERSION;
  d->mode = SNAP_AI_MODE_BYPASS; d->flow_scale = 1.0f;
  d->search_radius_scale = 1.0f; d->confidence_threshold = 0.55f;
  d->refinement_passes = h ? h->cfg.max_refinement_passes : 2;
  d->reuse_previous_flow = 0; d->run_bidirectional_flow = 1;
  d->run_artifact_check = 0; d->estimated_compute_saving = 0.0f; d->frame_id = id;
}
}

extern "C" {
void snap_ai_default_config(SnapAIConfig* c) {
  if (!c) return;
  *c = {};
  c->struct_size = sizeof(*c); c->api_version = SNAP_AI_API_VERSION;
  c->profile = SNAP_AI_PROFILE_BALANCED; c->enabled = 1;
  c->target_gpu_budget_ms = 5.0f; c->min_flow_scale = 0.60f;
  c->max_flow_scale = 1.0f; c->min_refinement_passes = 1;
  c->max_refinement_passes = 3; c->hysteresis = 0.12f; c->warmup_frames = 8;
}

SnapAIHandle* snap_ai_create(const SnapAIConfig* in) {
  SnapAIConfig c{}; snap_ai_default_config(&c);
  if (in) {
    if (in->struct_size < sizeof(SnapAIConfig) || in->api_version != SNAP_AI_API_VERSION ||
        !valid_profile(in->profile) || !finite(in->target_gpu_budget_ms) ||
        !finite(in->min_flow_scale) || !finite(in->max_flow_scale) ||
        !finite(in->hysteresis) || in->target_gpu_budget_ms <= 0 ||
        in->min_flow_scale <= 0 || in->max_flow_scale < in->min_flow_scale ||
        in->max_flow_scale > 1.5f || in->min_refinement_passes > in->max_refinement_passes ||
        in->max_refinement_passes > 16) return nullptr;
    c = *in;
  }
  auto* h = new (std::nothrow) SnapAIHandle();
  if (!h) return nullptr;
  h->cfg = c; h->warmup_left = c.warmup_frames;
  h->stats.struct_size = sizeof(SnapAIStats);
  return h;
}
void snap_ai_destroy(SnapAIHandle* h) { delete h; }

SnapAIStatus snap_ai_update(SnapAIHandle* h, const SnapAIFrameMetrics* m, SnapAIDecision* d) {
  if (!h || !m || !d || m->struct_size < sizeof(*m) || d->struct_size < sizeof(*d))
    return SNAP_AI_ERROR_INVALID_ARGUMENT;
  std::lock_guard<std::mutex> guard(h->mutex);
  const uint64_t id = m->frame_id;
  const bool ok = m->telemetry_valid && finite(m->gpu_time_ms) &&
    finite(m->motion_magnitude) && finite(m->motion_complexity) &&
    finite(m->flow_confidence) && finite(m->gpu_pressure) && finite(m->artifact_risk) &&
    m->gpu_time_ms >= 0 && m->motion_magnitude >= 0 && m->motion_complexity >= 0 &&
    m->flow_confidence >= 0 && m->gpu_pressure >= 0 && m->artifact_risk >= 0;
  ++h->stats.frames_seen;
  if (!h->cfg.enabled || !ok || m->scene_cut || (h->have_frame && id <= h->last_frame_id)) {
    bypass(h, id, d); ++h->stats.bypass_frames;
    if (!ok || m->scene_cut) {
      h->warmup_left = h->cfg.warmup_frames;
      h->complexity = 0; h->confidence = 1; h->pressure = 0;
    }
    h->last_frame_id = id; h->have_frame = true;
    return SNAP_AI_OK;
  }

  const float raw_c = clamp01(m->motion_complexity);
  const float raw_q = clamp01(m->flow_confidence);
  const float raw_p = clamp01(std::max(m->gpu_pressure,
    m->gpu_time_ms / std::max(0.1f, h->cfg.target_gpu_budget_ms)));
  const float risk = clamp01(m->artifact_risk);
  constexpr float a = 0.25f;
  h->complexity += a * (raw_c - h->complexity);
  h->confidence += a * (raw_q - h->confidence);
  h->pressure += a * (raw_p - h->pressure);

  if (h->warmup_left) {
    --h->warmup_left; bypass(h, id, d); ++h->stats.bypass_frames;
    h->last_frame_id = id; h->have_frame = true; return SNAP_AI_OK;
  }

  float scale = 1.0f, radius = 1.0f, threshold = 0.55f;
  uint32_t passes = h->cfg.max_refinement_passes;
  bool reuse = false, artifact = false;
  SnapAIMode mode = SNAP_AI_MODE_ADAPTIVE;
  if (h->cfg.profile == SNAP_AI_PROFILE_PERFORMANCE) {
    scale = 0.76f; radius = 0.78f; threshold = 0.62f;
    passes = h->cfg.min_refinement_passes;
  } else if (h->cfg.profile == SNAP_AI_PROFILE_QUALITY) {
    scale = 0.96f; radius = 1.12f; threshold = 0.42f;
  }
  const float uncertainty = clamp01((1.0f - h->confidence) * 0.65f + risk * 0.35f);
  const float easy = (1.0f - h->complexity) * h->confidence * (1.0f - risk);
  const float hard = clamp01(h->complexity * 0.55f + uncertainty * 0.45f);
  if (easy > 0.52f) {
    scale -= 0.20f * easy;
    if (passes > h->cfg.min_refinement_passes) --passes;
    reuse = easy > 0.70f; mode = SNAP_AI_MODE_CONSERVATIVE;
  }
  if (hard > 0.42f) {
    scale += 0.12f * hard; radius += 0.22f * hard;
    if (passes < h->cfg.max_refinement_passes) ++passes;
    artifact = uncertainty > 0.30f || risk > 0.25f; mode = SNAP_AI_MODE_QUALITY;
  }
  if (h->pressure > 0.90f) scale = std::min(scale, 0.70f);
  else if (h->pressure > 0.75f) scale = std::min(scale, 0.82f);
  else if (h->pressure < 0.45f && uncertainty > 0.25f) scale = std::max(scale, 0.92f);

  scale = clampf(scale, h->cfg.min_flow_scale, h->cfg.max_flow_scale);
  radius = clampf(radius, 0.5f, 1.5f);
  threshold = clampf(threshold + (h->confidence < 0.5f ? -0.08f : 0.04f), 0.2f, 0.9f);
  const float step = clampf(h->cfg.hysteresis, 0.02f, 0.5f);
  scale = clampf(scale, h->previous_scale - step, h->previous_scale + step);
  scale = clampf(scale, h->cfg.min_flow_scale, h->cfg.max_flow_scale);

  d->struct_size = sizeof(*d); d->api_version = SNAP_AI_API_VERSION; d->mode = mode;
  d->flow_scale = scale; d->search_radius_scale = radius;
  d->confidence_threshold = threshold; d->refinement_passes = passes;
  d->reuse_previous_flow = reuse ? 1u : 0u;
  d->run_bidirectional_flow = (uncertainty > 0.48f || risk > 0.35f) ? 1u : 0u;
  d->run_artifact_check = artifact ? 1u : 0u;
  d->estimated_compute_saving = clamp01((1.0f - scale) * 0.75f); d->frame_id = id;

  h->previous_scale = scale; h->last_frame_id = id; h->have_frame = true;
  ++h->stats.adaptive_frames;
  const float n = static_cast<float>(h->stats.adaptive_frames);
  h->stats.average_motion_complexity += (raw_c - h->stats.average_motion_complexity) / n;
  h->stats.average_flow_confidence += (raw_q - h->stats.average_flow_confidence) / n;
  h->stats.average_gpu_pressure += (raw_p - h->stats.average_gpu_pressure) / n;
  h->stats.last_flow_scale = scale;
  return SNAP_AI_OK;
}

SnapAIStatus snap_ai_set_profile(SnapAIHandle* h, SnapAIProfile p) {
  if (!h || !valid_profile(p)) return SNAP_AI_ERROR_INVALID_ARGUMENT;
  std::lock_guard<std::mutex> guard(h->mutex); h->cfg.profile = p; return SNAP_AI_OK;
}
SnapAIStatus snap_ai_set_enabled(SnapAIHandle* h, uint32_t enabled) {
  if (!h) return SNAP_AI_ERROR_INVALID_ARGUMENT;
  std::lock_guard<std::mutex> guard(h->mutex);
  h->cfg.enabled = enabled ? 1u : 0u; h->warmup_left = h->cfg.warmup_frames; return SNAP_AI_OK;
}
SnapAIStatus snap_ai_get_stats(SnapAIHandle* h, SnapAIStats* s) {
  if (!h || !s || s->struct_size < sizeof(*s)) return SNAP_AI_ERROR_INVALID_ARGUMENT;
  std::lock_guard<std::mutex> guard(h->mutex); *s = h->stats;
  s->struct_size = sizeof(*s); return SNAP_AI_OK;
}
uint32_t snap_ai_get_api_version(void) { return SNAP_AI_API_VERSION; }
}
