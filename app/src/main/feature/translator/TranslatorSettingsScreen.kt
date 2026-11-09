package com.winlator.cmod.feature.translator

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.media.projection.MediaProjectionManager
import android.net.Uri
import android.provider.Settings
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import com.winlator.cmod.feature.settings.SettingsNavBridge

private const val PREFS = "wintranslator"
private const val PROVIDER = "provider"
private const val ENDPOINT = "endpoint"
private const val API_KEY = "api_key"
private const val SOURCE = "source"
private const val TARGET = "target"
private const val MODEL = "model"

@Composable
fun TranslatorSettingsScreen(bridge: SettingsNavBridge) {
    val context = LocalContext.current
    val prefs = remember { context.getSharedPreferences(PREFS, Context.MODE_PRIVATE) }
    var provider by remember { mutableStateOf(prefs.getString(PROVIDER, "deepl") ?: "deepl") }
    var endpoint by remember { mutableStateOf(prefs.getString(ENDPOINT, "https://api-free.deepl.com/v2/translate") ?: "") }
    var apiKey by remember { mutableStateOf(prefs.getString(API_KEY, "") ?: "") }
    var source by remember { mutableStateOf(prefs.getString(SOURCE, "AUTO") ?: "AUTO") }
    var target by remember { mutableStateOf(prefs.getString(TARGET, "RU") ?: "RU") }
    var model by remember { mutableStateOf(prefs.getString(MODEL, "gpt-4o-mini") ?: "gpt-4o-mini") }
    var status by remember { mutableStateOf("Готов к настройке") }

    val captureLauncher = rememberLauncherForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
        if (result.resultCode == Activity.RESULT_OK && result.data != null) {
            TranslatorOverlayService.start(context, result.resultCode, result.data!!)
            status = "Захват экрана запущен. Нажми плавающую кнопку «译» в игре."
        } else status = "Разрешение на захват экрана не выдано"
    }
    fun save() {
        prefs.edit().putString(PROVIDER, provider).putString(ENDPOINT, endpoint)
            .putString(API_KEY, apiKey).putString(SOURCE, source)
            .putString(TARGET, target).putString(MODEL, model).apply()
        status = "Настройки сохранены"
    }

    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(20.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
        Text("WinTranslator AI", style = MaterialTheme.typography.headlineMedium)
        Text("Игровые диалоги и субтитры: локальный OCR, затем онлайн-перевод. В игру не внедряются DLL и не изменяются файлы.", style = MaterialTheme.typography.bodyMedium)
        Card { Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("Провайдер", style = MaterialTheme.typography.titleMedium)
            listOf("deepl" to "DeepL", "libre" to "LibreTranslate", "google" to "Google Cloud Translation", "openai" to "OpenAI-compatible AI").forEach { (id, label) ->
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                    Text(label); RadioButton(selected = provider == id, onClick = {
                        provider = id
                        endpoint = when(id) {
                            "deepl" -> "https://api-free.deepl.com/v2/translate"
                            "libre" -> "https://libretranslate.com/translate"
                            "google" -> "https://translation.googleapis.com/language/translate/v2"
                            else -> "https://api.openai.com/v1/chat/completions"
                        }
                    })
                }
            }
            OutlinedTextField(endpoint, { endpoint = it }, label = { Text("HTTPS API endpoint") }, modifier = Modifier.fillMaxWidth(), singleLine = true)
            OutlinedTextField(apiKey, { apiKey = it }, label = { Text("API key") }, modifier = Modifier.fillMaxWidth(), singleLine = true)
            if (provider == "openai") OutlinedTextField(model, { model = it }, label = { Text("AI model") }, modifier = Modifier.fillMaxWidth(), singleLine = true)
        } }
        Card { Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("Языки", style = MaterialTheme.typography.titleMedium)
            OutlinedTextField(source, { source = it.uppercase() }, label = { Text("Исходный язык (AUTO или EN)") }, modifier = Modifier.fillMaxWidth(), singleLine = true)
            OutlinedTextField(target, { target = it.uppercase() }, label = { Text("Язык перевода (например RU)") }, modifier = Modifier.fillMaxWidth(), singleLine = true)
        } }
        Button(onClick = { save() }, modifier = Modifier.fillMaxWidth()) { Text("Сохранить настройки") }
        Button(onClick = {
            save()
            if (!Settings.canDrawOverlays(context)) {
                context.startActivity(Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION, Uri.parse("package:" + context.packageName)))
                status = "Разреши отображение поверх приложений и повтори запуск"
            } else {
                val manager = context.getSystemService(Context.MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
                captureLauncher.launch(manager.createScreenCaptureIntent())
            }
        }, modifier = Modifier.fillMaxWidth()) { Text("Запустить игровой переводчик") }
        OutlinedButton(onClick = { context.stopService(Intent(context, TranslatorOverlayService::class.java)); status = "Переводчик остановлен" }, modifier = Modifier.fillMaxWidth()) { Text("Остановить переводчик") }
        Text(status, style = MaterialTheme.typography.bodyMedium)
        Text("Android запросит разрешение на захват экрана. Кадры остаются на устройстве; распознанные OCR строки отправляются выбранному HTTPS-провайдеру. API-ключ хранится в приватных настройках приложения.", style = MaterialTheme.typography.bodySmall)
    }
}
