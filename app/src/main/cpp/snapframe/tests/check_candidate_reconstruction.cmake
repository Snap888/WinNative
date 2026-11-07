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
        "metrics.flow_confidence >= 0.12f"
        "metrics.artifact_risk <= 0.20f"
        "metrics.gpu_pressure>1.15f"
        "metrics.gpu_pressure<0.98f")
    string(FIND "${host}" "${token}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "Snap AI motion-aware quality gate is missing: ${token}")
    endif()
endforeach()
file(READ "${AI_FILE}" ai_source)
file(READ "${FLOW_SHADER_FILE}" flow_shader)
foreach(token IN ITEMS
        "search_radius_scale=fmaxf"
        "snap_block_match_flow_comp_size,3,3,12"
        "searchRadiusScale"
        "coarseRadius = clamp"
        "radius += 0.45f * hard")
    string(FIND "${host}${flow_shader}${ai_source}" "${token}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "Adaptive flow search contract is missing: ${token}")
    endif()
endforeach()
message(STATUS "SnapFrame candidate, AI quality gate and stable flow viewer contracts are present")
