file(READ "${SOURCE_FILE}" source)

# A failed Acquire is not a Present result. Promoting a timeout to OUT_OF_DATE
# causes the compositor to tear down a healthy Wayland swapchain every frame.
string(FIND "${source}" "pr = VK_ERROR_OUT_OF_DATE_KHR;" fake_out_of_date)
if(NOT fake_out_of_date EQUAL -1)
    message(FATAL_ERROR "Acquire failure must not be rewritten as VK_ERROR_OUT_OF_DATE_KHR")
endif()

string(FIND "${source}" "if (k < ngen && (ark == VK_TIMEOUT || ark == VK_NOT_READY))" skip_generated)
if(skip_generated EQUAL -1)
    message(FATAL_ERROR "Generated-slot timeout must skip remaining generated slots without rebuilding")
endif()

string(FIND "${source}" "k = ngen - 1;" source_fallback)
if(source_fallback EQUAL -1)
    message(FATAL_ERROR "Generated-slot timeout must continue to the source-frame slot")
endif()

string(FIND "${source}" "timed_wait(frame_fence, UINT64_MAX)" selected_fence_wait)
if(selected_fence_wait EQUAL -1)
    message(FATAL_ERROR "Early-exit path must wait for the selected frame fence, including SnapFrame readback")
endif()

string(FIND "${source}" "QueueSubmit(g_queue, 1, &si, frame_fence)" selected_fence_submit)
if(selected_fence_submit EQUAL -1)
    message(FATAL_ERROR "Early-exit path must signal the selected frame fence")
endif()
