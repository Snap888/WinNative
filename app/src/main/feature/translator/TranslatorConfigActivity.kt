package com.winlator.cmod.feature.translator

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.material3.MaterialTheme
import com.winlator.cmod.feature.settings.SettingsNavBridge

class TranslatorConfigActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent {
            MaterialTheme {
                TranslatorSettingsScreen(SettingsNavBridge())
            }
        }
    }
}
