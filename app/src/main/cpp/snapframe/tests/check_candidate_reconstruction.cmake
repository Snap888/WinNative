if(NOT DEFINED SOURCE_DIR OR NOT DEFINED HOST_FILE OR NOT DEFINED RENDERER_FILE OR NOT DEFINED AI_FILE OR NOT DEFINED FLOW_SHADER_FILE)
    message(FATAL_ERROR "SOURCE_DIR, HOST_FILE, RENDERER_FILE, AI_FILE and FLOW_SHADER_FILE are required")
endif()

set(required_shaders
    candidate_warp.comp
    candidate_score.comp
    candidate_reconstruct.comp
    candidate_reconstruct_rgba8.comp)
foreach(shader IN LISTS required_shaders)
    set(path "${SOURCE_DIR}/shaders/${shader}")
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "Missing candidate shader: ${path}")
    endif()
    file(READ "${path}" source)
    if(NOT source MATCHES "local_size_x = 8")
        message(FATAL_ERROR "Unexpected workgroup contract in ${shader}")
    endif()
    if(source MATCHES "GL_AMD_gpu_shader_half_float|GL_NV_gpu_shader5|float16_t")
        message(FATAL_ERROR "Candidate shader must not require optional FP16 arithmetic extensions: ${shader}")
    endif()
endforeach()

file(READ "${HOST_FILE}" host)
foreach(token IN ITEMS
        "candidate_warp"
        "local_flow"
        "motion_fusion"
        ".maxSets=40"
        "COMBINED_IMAGE_SAMPLER,192"
        "snap_flow_consistency_comp_size,5,27,8"
        "fused_forward"
        "fused_backward"
        "candidate_score"
        "candidate_reconstruct8"
        "candidate_reconstruct16"
        "candidate_reconstruction_enabled"
        "barrier_compute(cmd)"
        "SF_LOGI"
        "SNAP_AI_MODE_BYPASS"
        "manual_flow_percent"
        "n == UINT32_MAX"
        "if(debug_only)copy_history")
    string(FIND "${host}" "${token}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "SnapFrame host is missing candidate integration token: ${token}")
    endif()
endforeach()
file(READ "${RENDERER_FILE}" renderer)
if(NOT renderer MATCHES "if \\(use_snap && r->snap_debug_flow\\)")
    message(FATAL_ERROR "SnapFrame flow viewer must not depend on generated-frame acquire success")
endif()
if(renderer MATCHES "if \\(use_snap && r->snap_debug_flow && gen_count > 0\\)")
    message(FATAL_ERROR "SnapFrame flow viewer is incorrectly gated by gen_count")
endif()
string(FIND "${renderer}" "snap_process_count = UINT32_MAX" debug_only_found)
if(debug_only_found EQUAL -1)
    message(FATAL_ERROR "Renderer must request flow-only processing when no generated frame is acquired")
endif()

foreach(token IN ITEMS
        "target_gpu_budget_ms=7.0f"
        "metrics.gpu_pressure=total/7.0f"
        "metrics.motion_magnitude >= 0.12f"
        "metrics.motion_complexity >= 0.18f"
        "metrics.flow_confidence >= 0.18f"
        "metrics.artifact_risk <= 0.10f"
        "metrics.gpu_pressure>1.15f"
        "metrics.gpu_pressure<0.98f")
    string(FIND "${host}" "${token}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "Snap AI motion-aware quality gate is missing: ${token}")
    endif()
endforeach()
file(READ "${AI_FILE}" ai_source)
file(READ "${FLOW_SHADER_FILE}" flow_shader)
file(READ "${SOURCE_DIR}/shaders/flow_consistency.comp" consistency_shader)
file(READ "${SOURCE_DIR}/shaders/flow_metrics.comp" metrics_shader)
foreach(token IN ITEMS
        "raw_q < 0.18f"
        "risk > 0.20f"
        "unreliable_flow"
        "radius = 1.0f"
        "artifact = true"
        "raw_q - 0.18f")
    string(FIND "${ai_source}" "${token}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "Snap AI low-confidence safety policy is missing: ${token}")
    endif()
endforeach()
foreach(token IN ITEMS
        "search_radius_scale=fmaxf"
        "snap_block_match_flow_comp_size,3,3,12"
        "searchRadiusScale"
        "coarseRadius = clamp"
        "radius += 0.30f * hard")
    string(FIND "${host}${flow_shader}${ai_source}" "${token}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "Adaptive flow search contract is missing: ${token}")
    endif()
endforeach()
foreach(token IN ITEMS
        "photometricVisibility"
        "photometricError"
        "previousLuma"
        "currentLuma"
        "smoothstep(0.08, 0.28")
    string(FIND "${consistency_shader}" "${token}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "SnapFrame temporal visibility confidence is missing: ${token}")
    endif()
endforeach()
foreach(token IN ITEMS
        "globalMotion"
        "localMotion"
        "divergence"
        "max(forwardConfidence * (1.0 - consistency), divergence)")
    string(FIND "${metrics_shader}" "${token}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "Snap AI global/local divergence telemetry is missing: ${token}")
    endif()
endforeach()
message(STATUS "SnapFrame candidate, temporal visibility, divergence telemetry, AI quality gate and stable flow viewer contracts are present")

# Both presentation backends must route SnapFrame to the same Vulkan implementation.
if(DEFINED WAYLAND_BRIDGE_FILE AND DEFINED WAYLAND_ENGINE_FILE AND DEFINED X11_RENDERER_FILE AND DEFINED WAYLAND_PRESENT_FILE AND DEFINED X11_STATE_FILE)
    file(READ "${WAYLAND_BRIDGE_FILE}" wayland_bridge)
    file(READ "${WAYLAND_ENGINE_FILE}" wayland_engine)
    file(READ "${X11_RENDERER_FILE}" x11_renderer)
    file(READ "${WAYLAND_PRESENT_FILE}" wayland_present)
    file(READ "${X11_STATE_FILE}" x11_state)
    if(NOT wayland_bridge MATCHES "vkp_framegen_acquire_timeout_ns" OR NOT wayland_present MATCHES "vkp_framegen_acquire_timeout_ns\\(\\)")
        message(FATAL_ERROR "Wayland framegen acquire must use the bounded refresh-aware timeout")
    endif()
    if(NOT wayland_bridge MATCHES "16000000ULL" OR NOT wayland_bridge MATCHES "4000000ULL")
        message(FATAL_ERROR "Wayland framegen acquire timeout must be clamped to 4..16 ms")
    endif()
    if(NOT x11_state MATCHES "VK_FRAMEGEN_ACQUIRE_TIMEOUT_NS 8000000ULL" OR NOT x11_state MATCHES "VK_FRAMEGEN_ACQUIRE_TIMEOUT_MAX_NS 16000000ULL")
        message(FATAL_ERROR "X11 framegen acquire budget must be bounded at 8..16 ms")
    endif()
    foreach(token IN ITEMS
            "vk.GetImageMemoryRequirements && vk.AllocateMemory"
            "vk.BindImageMemory && vk.CmdPipelineBarrier && vk.DeviceWaitIdle"
            "bind_result != VK_SUCCESS"
            "g_ring_n = i + 1")
        string(FIND "${wayland_bridge}" "${token}" found)
        if(found EQUAL -1)
            message(FATAL_ERROR "Wayland Vulkan ring safety contract is missing: ${token}")
        endif()
    endforeach()
    if(wayland_engine MATCHES "VK_FORMAT_R16G16_SFLOAT")
        message(FATAL_ERROR "Wayland SnapFrame capability gate must not require the unused RG16F format")
    endif()
    foreach(token IN ITEMS
            "VK_FORMAT_R16_SFLOAT"
            "VK_FORMAT_R16G16B16A16_SFLOAT"
            "VK_FORMAT_R8G8B8A8_UNORM")
        string(FIND "${wayland_engine}" "${token}" found)
        if(found EQUAL -1)
            message(FATAL_ERROR "Wayland SnapFrame is missing a required image format capability check: ${token}")
        endif()
    endforeach()
    foreach(token IN ITEMS
            "snap_frame_vk_process(h->vk"
            "snap_frame_process(g_snap_core"
            "fge_set_candidate_reconstruction"
            "SnapFrame process failed")
        string(FIND "${wayland_engine}" "${token}" found)
        if(found EQUAL -1)
            message(FATAL_ERROR "Wayland SnapFrame engine integration is missing: ${token}")
        endif()
    endforeach()
    foreach(token IN ITEMS
            "snap_frame_vk_process(r->snap"
            "snap_frame_vk_generate(r->snap"
            "nativeSetSnapFrameCandidateReconstruction"
            "snap_frame_vk_prepare(r->snap")
        string(FIND "${x11_renderer}" "${token}" found)
        if(found EQUAL -1)
            message(FATAL_ERROR "X11 SnapFrame integration is missing: ${token}")
        endif()
    endforeach()
endif()
