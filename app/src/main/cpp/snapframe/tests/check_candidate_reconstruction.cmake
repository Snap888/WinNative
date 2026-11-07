if(NOT DEFINED SOURCE_DIR OR NOT DEFINED HOST_FILE)
    message(FATAL_ERROR "SOURCE_DIR and HOST_FILE are required")
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
        "barrier_compute(cmd)")
    string(FIND "${host}" "${token}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "SnapFrame host is missing candidate integration token: ${token}")
    endif()
endforeach()
message(STATUS "SnapFrame candidate shader and host contract is present")
