package com.winlator.cmod.shared.framegen

import com.winlator.cmod.R

enum class FrameGenEngine(val labelRes: Int) {
    OFF(R.string.session_drawer_frame_generation_engine_off),
    LSFG(R.string.session_drawer_frame_generation_engine_lsfg),
    DIS(R.string.session_drawer_frame_generation_engine_dis),
    SNAPFRAME(R.string.session_drawer_frame_generation_engine_snapframe),
    ;

    companion object {
        @JvmStatic
        fun of(
            lsfgEnabled: Boolean,
            disEnabled: Boolean,
            snapFrameEnabled: Boolean = false,
        ): FrameGenEngine =
            when {
                snapFrameEnabled -> SNAPFRAME
                disEnabled -> DIS
                lsfgEnabled -> LSFG
                else -> OFF
            }
    }
}
