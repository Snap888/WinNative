file(READ "${JAVA_FILE}" java_source)
file(READ "${NATIVE_FILE}" native_source)

# Java must pass saturation in slider percent (100 means 100%), because native
# converts percent to shader units exactly once in vkp_effects_set_screen().
string(FIND "${java_source}" "float effectiveSaturation = (float) saturation;" java_percent_units)
if(java_percent_units EQUAL -1)
    message(FATAL_ERROR "Wayland Java bridge must pass saturation in percent units")
endif()

string(FIND "${java_source}" "float effectiveSaturation = saturation / 100.0f;" double_normalization)
if(NOT double_normalization EQUAL -1)
    message(FATAL_ERROR "Wayland Java bridge normalizes saturation before native normalizes it again")
endif()

string(FIND "${native_source}" "g_set.saturation = clampf(saturation / 100.0f, 0.0f, 2.0f);" native_conversion)
if(native_conversion EQUAL -1)
    message(FATAL_ERROR "Native saturation conversion contract changed; review Java/native units together")
endif()

string(FIND "${java_source}" "gammaPercent / 100.0f, effectiveSaturation" native_call)
if(native_call EQUAL -1)
    message(FATAL_ERROR "Wayland screen-effects call must pass the unnormalized saturation value")
endif()
