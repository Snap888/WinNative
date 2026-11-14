add_library(framelab OBJECT src/anr_frame_lab.c src/anr_frame_lab_metrics.c)
target_include_directories(framelab PUBLIC include)
target_compile_features(framelab PUBLIC c_std_11)
target_compile_options(framelab PRIVATE -Wall -Wextra -Werror)
