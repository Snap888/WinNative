cmake_minimum_required(VERSION 3.16)
project(snapframe_contract_tests LANGUAGES CXX)
enable_testing()
add_executable(snapframe_contract_tests test_snap_frame_vk_contract.cpp)
target_compile_features(snapframe_contract_tests PRIVATE cxx_std_17)
target_compile_definitions(snapframe_contract_tests PRIVATE SNAP_FRAME_VK_CONTRACT_STANDALONE=1)
target_include_directories(snapframe_contract_tests PRIVATE ../include)
add_test(NAME snapframe_contract_tests COMMAND snapframe_contract_tests)
add_test(NAME snapframe_wsi_ownership
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_FILE=${CMAKE_CURRENT_LIST_DIR}/../src/snap_frame_vk.c
        -P ${CMAKE_CURRENT_LIST_DIR}/check_wsi_ownership.cmake)

add_test(NAME snapframe_candidate_reconstruction_contract
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${CMAKE_CURRENT_LIST_DIR}/..
        -DHOST_FILE=${CMAKE_CURRENT_LIST_DIR}/../src/snap_frame_vk.c
        -P ${CMAKE_CURRENT_LIST_DIR}/check_candidate_reconstruction.cmake)
