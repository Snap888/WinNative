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
    var provider by remember { mutableStateOf(prefs.getString(PROVIDER, "libre") ?: "libre") }
    var endpoint by remember { mutableStateOf(prefs.getString(ENDPOINT, "https://libretranslate.com/translate") ?: "") }
    var apiKey by remember { mutableStateOf(prefs.getString(API_KEY, "") ?: "") }
    var source by remember { mutableStateOf(prefs.getString(SOURCE, "AUTO") ?: "AUTO") }
    var target by remember { mutableStateOf(prefs.getString(TARGET, "RU") ?: "RU") }
    var model by remember { mutableStateOf(prefs.getString(MODEL, "gpt-4o-mini") ?: "gpt-4o-mini") }
    var status by remember { mutableStateOf("Переводчик выключен") }
    var pendingCapture by remember { mutableStateOf(false) }

    fun save() {
        prefs.edit().putString(PROVIDER, provider).putString(ENDPOINT, endpoint.trim())
            .putString(API_KEY, apiKey.trim()).putString(SOURCE, source.trim().uppercase())
            .putString(TARGET, target.trim().uppercase()).putString(MODEL, model.trim()).apply()
        status = "Настройки сохранены"
    }

    val captureLauncher = rememberLauncherForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
        if (result.resultCode == Activity.RESULT_OK && result.data != null) {
            TranslatorOverlayService.start(context, result.resultCode, result.data!!)
            status = "Захват активен. Вернись в игру и нажми плавающую кнопку «ПЕРЕВОД»."
        } else status = "Захват не разрешён. Нажми «Запустить переводчик» и подтверди запрос Android."
    }
    val overlayPermissionLauncher = rememberLauncherForActivityResult(ActivityResultContracts.StartActivityForResult()) {
        if (Settings.canDrawOverlays(context) && pendingCapture) {
            pendingCapture = false
            val manager = context.getSystemService(Context.MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
            captureLauncher.launch(manager.createScreenCaptureIntent())
        } else if (!Settings.canDrawOverlays(context)) {
            pendingCapture = false
            status = "Нет разрешения «Поверх других приложений». Без него игровая панель не появится."
        }
    }

    Column(
        Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(18.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        Text("Игровой переводчик", style = MaterialTheme.typography.headlineMedium)
        Text("Захватывает изображение игры, распознаёт надписи на устройстве и показывает перевод поверх игры. Файлы игры не изменяются.", style = MaterialTheme.typography.bodyMedium)

        Card {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("Быстрый запуск", style = MaterialTheme.typography.titleMedium)
                Text("1. Сохрани провайдера и языки. 2. Разреши отображение поверх приложений. 3. Подтверди системный захват экрана. 4. В игре нажми плавающую кнопку «ПЕРЕВОД».", style = MaterialTheme.typography.bodySmall)
                Button(onClick = {
                    save()
                    if (!Settings.canDrawOverlays(context)) {
                        pendingCapture = true
                        status = "Разреши WinNative отображаться поверх других приложений."
                        overlayPermissionLauncher.launch(Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION, Uri.parse("package:" + context.packageName)))
                    } else {
                        val manager = context.getSystemService(Context.MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
                        captureLauncher.launch(manager.createScreenCaptureIntent())
                    }
                }, modifier = Modifier.fillMaxWidth()) { Text("ЗАПУСТИТЬ ПЕРЕВОД В ИГРЕ") }
                OutlinedButton(onClick = {
                    context.stopService(Intent(context, TranslatorOverlayService::class.java))
                    status = "Переводчик остановлен"
                }, modifier = Modifier.fillMaxWidth()) { Text("Остановить переводчик") }
                Text(status, style = MaterialTheme.typography.bodyMedium)
            }
        }

        Card {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("Служба перевода", style = MaterialTheme.typography.titleMedium)
                listOf(
                    "libre" to "LibreTranslate (может требовать ключ)",
                    "deepl" to "DeepL",
                    "google" to "Google Cloud Translation",
                    "openai" to "OpenAI-compatible AI"
                ).forEach { (id, label) ->
                    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                        Text(label, modifier = Modifier.weight(1f))
                        RadioButton(selected = provider == id, onClick = {
                            provider = id
                            endpoint = when (id) {
                                "deepl" -> "https://api-free.deepl.com/v2/translate"
                                "libre" -> "https://libretranslate.com/translate"
                                "google" -> "https://translation.googleapis.com/language/translate/v2"
                                else -> "https://api.openai.com/v1/chat/completions"
                            }
                        })
                    }
                }
                OutlinedTextField(endpoint, { endpoint = it }, label = { Text("HTTPS API endpoint") }, modifier = Modifier.fillMaxWidth(), singleLine = true)
                OutlinedTextField(apiKey, { apiKey = it }, label = { Text("API key (если требуется)") }, modifier = Modifier.fillMaxWidth(), singleLine = true)
                if (provider == "openai") OutlinedTextField(model, { model = it }, label = { Text("AI model") }, modifier = Modifier.fillMaxWidth(), singleLine = true)
            }
        }

        Card {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("Языки перевода", style = MaterialTheme.typography.titleMedium)
                OutlinedTextField(source, { source = it.uppercase().take(8) }, label = { Text("Исходный язык (AUTO, EN, JA…)") }, modifier = Modifier.fillMaxWidth(), singleLine = true)
                OutlinedTextField(target, { target = it.uppercase().take(8) }, label = { Text("Переводить на (RU, EN…)") }, modifier = Modifier.fillMaxWidth(), singleLine = true)
                Button(onClick = { save() }, modifier = Modifier.fillMaxWidth()) { Text("Сохранить настройки") }
            }
        }

        Text("Важно: распознавание выполняется на устройстве. В сеть отправляется только распознанный текст. Для DeepL, Google и большинства AI API нужен собственный ключ и доступ в интернет. Перевод отображается отдельной панелью, он не подменяет текст внутри Windows-процесса.", style = MaterialTheme.typography.bodySmall)
    }
}
