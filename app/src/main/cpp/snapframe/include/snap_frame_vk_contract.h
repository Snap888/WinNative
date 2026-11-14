#ifndef SNAP_FRAME_VK_CONTRACT_H
#define SNAP_FRAME_VK_CONTRACT_H
#include <stdint.h>
#ifdef SNAP_FRAME_VK_CONTRACT_STANDALONE
typedef int32_t VkFormat;
enum { VK_FORMAT_R8G8B8A8_UNORM = 37, VK_FORMAT_B8G8R8A8_UNORM = 44, VK_FORMAT_R16G16B16A16_SFLOAT = 97 };
#else
#include <vulkan/vulkan.h>
#endif

enum {
    SNAP_FRAME_CANDIDATE_AI_AUTO = 0u,
    SNAP_FRAME_CANDIDATE_FORCE_OFF = 1u,
    SNAP_FRAME_CANDIDATE_FORCE_ON = 2u
};

static inline uint32_t snap_frame_vk_candidate_mode(uint32_t mode) {
    return mode <= SNAP_FRAME_CANDIDATE_FORCE_ON ? mode : SNAP_FRAME_CANDIDATE_AI_AUTO;
}

enum {
    SNAP_FRAME_DEBUG_MOTION = 0u,
    SNAP_FRAME_DEBUG_CONFIDENCE = 1u,
    SNAP_FRAME_DEBUG_CONSISTENCY = 2u,
    SNAP_FRAME_DEBUG_MODE_COUNT = 3u
};

static inline float snap_frame_vk_confident_inconsistency(float confidence, float consistency) {
    if (confidence < 0.0f) confidence = 0.0f;
    if (confidence > 1.0f) confidence = 1.0f;
    if (consistency < 0.0f) consistency = 0.0f;
    if (consistency > 1.0f) consistency = 1.0f;
    return confidence * (1.0f - consistency);
}

static inline uint32_t snap_frame_vk_debug_mode(uint32_t mode) {
    return mode < SNAP_FRAME_DEBUG_MODE_COUNT ? mode : SNAP_FRAME_DEBUG_MOTION;
}

enum {
    SNAP_FRAME_RESET_INITIAL = 1u << 0,
    SNAP_FRAME_RESET_RESIZE = 1u << 1,
    SNAP_FRAME_RESET_FORMAT = 1u << 2,
    SNAP_FRAME_RESET_SCENE_CUT = 1u << 3,
    SNAP_FRAME_RESET_ARM = 1u << 4,
    SNAP_FRAME_RESET_DEVICE = 1u << 5
};

typedef struct SnapFrameVkLifecycle {
    uint64_t device_generation;
    uint32_t width, height, armed, initialized, history_valid;
    VkFormat format;
} SnapFrameVkLifecycle;

static inline uint32_t snap_frame_vk_flow_extent(uint32_t max_w, uint32_t max_h,
                                                  uint32_t percent, uint32_t *out_w,
                                                  uint32_t *out_h) {
    if (percent < 25u) percent = 25u;
    if (percent > 100u) percent = 100u;
    uint32_t w = max_w * percent / 100u;
    uint32_t h = max_h * percent / 100u;
    if (!w) w = 1u;
    if (!h) h = 1u;
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return percent;
}

static inline float snap_frame_vk_phase_time(uint32_t index, uint32_t count) {
    if (!count || count > 3u || index >= count) return -1.0f;
    return (float)(index + 1u) / (float)(count + 1u);
}

static inline int snap_frame_vk_format_supported(VkFormat format) {
    return format == VK_FORMAT_R8G8B8A8_UNORM ||
           format == VK_FORMAT_R16G16B16A16_SFLOAT;
}

static inline uint32_t snap_frame_vk_lifecycle_update(
        SnapFrameVkLifecycle *state, uint64_t device_generation,
        uint32_t width, uint32_t height, VkFormat format,
        uint32_t armed, uint32_t scene_cut) {
    if (!state) return 0;
    uint32_t flags = 0;
    if (!state->initialized) flags |= SNAP_FRAME_RESET_INITIAL;
    else {
        if (state->device_generation != device_generation) flags |= SNAP_FRAME_RESET_DEVICE;
        if (state->width != width || state->height != height) flags |= SNAP_FRAME_RESET_RESIZE;
        if (state->format != format) flags |= SNAP_FRAME_RESET_FORMAT;
        if (state->armed != armed) flags |= SNAP_FRAME_RESET_ARM;
    }
    if (scene_cut) flags |= SNAP_FRAME_RESET_SCENE_CUT;
    if (flags) state->history_valid = 0;
    state->device_generation = device_generation;
    state->width = width; state->height = height;
    state->format = format; state->armed = armed;
    state->initialized = 1;
    return flags;
}
#endif
