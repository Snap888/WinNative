#include "snap_ai/snap_ai.h"
#include <cassert>
#include <cmath>

static SnapAIFrameMetrics make_metrics(uint64_t id, float motion, float complexity,
                                       float confidence, float risk) {
    SnapAIFrameMetrics m{};
    m.struct_size = sizeof(m);
    m.frame_id = id;
    m.gpu_time_ms = 1.5f;
    m.motion_magnitude = motion;
    m.motion_complexity = complexity;
    m.flow_confidence = confidence;
    m.gpu_pressure = 0.25f;
    m.artifact_risk = risk;
    m.telemetry_valid = 1;
    return m;
}

int main() {
    SnapAIConfig cfg{};
    snap_ai_default_config(&cfg);
    cfg.warmup_frames = 0;
    SnapAIHandle* ai = snap_ai_create(&cfg);
    assert(ai);

    SnapAIDecision d{};
    d.struct_size = sizeof(d);
    auto still = make_metrics(1, 0.001f, 0.004f, 0.04f, 0.006f);
    assert(snap_ai_update(ai, &still, &d) == SNAP_AI_OK);
    assert(d.mode == SNAP_AI_MODE_CONSERVATIVE);
    assert(d.bypass_reason == SNAP_AI_BYPASS_NONE);
    assert(d.conservative_reason == SNAP_AI_CONSERVATIVE_NONE);
    assert(d.reuse_previous_flow == 1u);
    assert(std::fabs(d.flow_scale - 1.0f) < 0.0001f);

    auto uncertain_motion = make_metrics(2, 0.30f, 0.20f, 0.04f, 0.45f);
    assert(snap_ai_update(ai, &uncertain_motion, &d) == SNAP_AI_OK);
    assert(d.mode == SNAP_AI_MODE_CONSERVATIVE);
    assert(d.reuse_previous_flow == 0u);
    assert((d.conservative_reason & SNAP_AI_CONSERVATIVE_LOW_CONFIDENCE) != 0u);
    assert((d.conservative_reason & SNAP_AI_CONSERVATIVE_HIGH_ARTIFACT_RISK) != 0u);

    snap_ai_destroy(ai);
    return 0;
}
