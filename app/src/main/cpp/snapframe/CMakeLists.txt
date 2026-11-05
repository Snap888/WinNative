set(SNAPFRAME_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(SNAPFRAME_SHADER_OUT "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${SNAPFRAME_SHADER_OUT}")
set(SNAPFRAME_SHADERS luma_downsample pyramid_downsample block_match_flow flow_consistency midpoint_warp_rgba8 midpoint_warp)
set(SNAPFRAME_SHADER_HEADERS "")
foreach(base ${SNAPFRAME_SHADERS})
    set(input "${SNAPFRAME_SHADER_DIR}/${base}.comp")
    set(var "snap_${base}_comp")
    set(spv "${SNAPFRAME_SHADER_OUT}/${var}.spv")
    set(hdr "${SNAPFRAME_SHADER_OUT}/${var}.spv.h")
    add_custom_command(
        OUTPUT "${hdr}"
        COMMAND "${GLSLC}" --target-env=vulkan1.1 -O "${input}" -o "${spv}"
        COMMAND "${CMAKE_COMMAND}" -DINPUT_FILE=${spv} -DOUTPUT_FILE=${hdr} -DVAR_NAME=${var} -P "${BIN2C_SCRIPT}"
        DEPENDS "${input}" "${BIN2C_SCRIPT}"
        COMMENT "Compiling SnapFrame shader ${base}.comp"
        VERBATIM
    )
    list(APPEND SNAPFRAME_SHADER_HEADERS "${hdr}")
endforeach()
add_custom_target(snapframe_shaders DEPENDS ${SNAPFRAME_SHADER_HEADERS})
add_library(snapframe_vk STATIC src/snap_frame_vk.c)
add_dependencies(snapframe_vk snapframe_shaders)
target_link_libraries(snapframe_vk PRIVATE snap_ai)
set_target_properties(snapframe_vk PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(snapframe_vk PRIVATE -Wall -Wextra)
target_compile_definitions(snapframe_vk PUBLIC VK_USE_PLATFORM_ANDROID_KHR)
target_include_directories(snapframe_vk PUBLIC include PRIVATE ${SNAPFRAME_SHADER_OUT} ${CMAKE_CURRENT_SOURCE_DIR}/../waylandcomp/src)
target_link_libraries(snapframe_vk PRIVATE log)

if(BUILD_TESTING AND NOT ANDROID)
    add_executable(snapframe_contract_tests tests/test_snap_frame_vk_contract.cpp)
    target_include_directories(snapframe_contract_tests PRIVATE include)
    add_test(NAME snapframe_contract_tests COMMAND snapframe_contract_tests)
    add_test(NAME snapframe_wsi_ownership
        COMMAND ${CMAKE_COMMAND}
            -DSOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/src/snap_frame_vk.c
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_wsi_ownership.cmake)
endif()
