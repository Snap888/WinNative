#include "snap_frame_vk_contract.h"
#include <cassert>
#include <cmath>
int main() {
    assert(std::fabs(snap_frame_vk_phase_time(0,2)-1.0f/3.0f)<0.0001f);
    assert(std::fabs(snap_frame_vk_phase_time(1,2)-2.0f/3.0f)<0.0001f);
    assert(std::fabs(snap_frame_vk_phase_time(0,3)-0.25f)<0.0001f);
    assert(std::fabs(snap_frame_vk_phase_time(1,3)-0.50f)<0.0001f);
    assert(std::fabs(snap_frame_vk_phase_time(2,3)-0.75f)<0.0001f);
    assert(snap_frame_vk_phase_time(3,3)<0.0f);
    uint32_t w=0,h=0;
    assert(snap_frame_vk_flow_extent(120,68,50,&w,&h)==50 && w==60 && h==34);
    assert(snap_frame_vk_flow_extent(120,68,1,&w,&h)==25 && w==30 && h==17);
    assert(snap_frame_vk_flow_extent(120,68,150,&w,&h)==100 && w==120 && h==68);
    assert(snap_frame_vk_format_supported(VK_FORMAT_R8G8B8A8_UNORM));
    assert(snap_frame_vk_format_supported(VK_FORMAT_R16G16B16A16_SFLOAT));
    assert(!snap_frame_vk_format_supported(VK_FORMAT_B8G8R8A8_UNORM));
    assert(snap_frame_vk_ai_flow_percent(75, SNAP_FRAME_AI_MODE_BYPASS, 0.25f) == 75);
    assert(snap_frame_vk_ai_flow_percent(75, 2u, 0.50f) == 38);
    assert(snap_frame_vk_ai_flow_percent(20, SNAP_FRAME_AI_MODE_BYPASS, 0.25f) == 25);
    assert(snap_frame_vk_ai_flow_percent(150, 2u, 1.50f) == 100);
    assert(snap_frame_vk_ai_flow_percent(80, 2u, -1.0f) == 80);
    assert(snap_frame_vk_debug_mode(0) == SNAP_FRAME_DEBUG_MOTION);
    assert(snap_frame_vk_debug_mode(1) == SNAP_FRAME_DEBUG_CONFIDENCE);
    assert(snap_frame_vk_debug_mode(2) == SNAP_FRAME_DEBUG_CONSISTENCY);
    assert(snap_frame_vk_debug_mode(3) == SNAP_FRAME_DEBUG_MOTION);
    assert(snap_frame_vk_debug_mode(UINT32_MAX) == SNAP_FRAME_DEBUG_MOTION);
    assert(std::fabs(snap_frame_vk_confident_inconsistency(0.9f, 0.2f) - 0.72f) < 0.0001f);
    assert(std::fabs(snap_frame_vk_confident_inconsistency(0.4f, 0.4f) - 0.24f) < 0.0001f);
    assert(snap_frame_vk_confident_inconsistency(-1.0f, 0.0f) == 0.0f);
    assert(snap_frame_vk_confident_inconsistency(2.0f, 0.0f) == 1.0f);
    assert(snap_frame_vk_confident_inconsistency(1.0f, 2.0f) == 0.0f);

    SnapFrameVkLifecycle state{};
    uint32_t reset=snap_frame_vk_lifecycle_update(&state,1,1920,1080,VK_FORMAT_R8G8B8A8_UNORM,1,0);
    assert(reset==SNAP_FRAME_RESET_INITIAL);
    state.history_valid=1;
    reset=snap_frame_vk_lifecycle_update(&state,1,2560,1440,VK_FORMAT_R8G8B8A8_UNORM,1,0);
    assert(reset&SNAP_FRAME_RESET_RESIZE); assert(!state.history_valid);
    state.history_valid=1;
    reset=snap_frame_vk_lifecycle_update(&state,1,2560,1440,VK_FORMAT_R16G16B16A16_SFLOAT,1,0);
    assert(reset&SNAP_FRAME_RESET_FORMAT); assert(!state.history_valid);
    state.history_valid=1;
    reset=snap_frame_vk_lifecycle_update(&state,1,2560,1440,VK_FORMAT_R16G16B16A16_SFLOAT,1,1);
    assert(reset&SNAP_FRAME_RESET_SCENE_CUT); assert(!state.history_valid);
    state.history_valid=1;
    reset=snap_frame_vk_lifecycle_update(&state,1,2560,1440,VK_FORMAT_R16G16B16A16_SFLOAT,0,0);
    assert(reset&SNAP_FRAME_RESET_ARM); assert(!state.history_valid);
    state.history_valid=1;
    reset=snap_frame_vk_lifecycle_update(&state,2,2560,1440,VK_FORMAT_R16G16B16A16_SFLOAT,0,0);
    assert(reset&SNAP_FRAME_RESET_DEVICE); assert(!state.history_valid);
    return 0;
}
