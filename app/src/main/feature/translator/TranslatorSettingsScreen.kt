package com.winlator.cmod.feature.translator

import android.content.Context
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import com.winlator.cmod.feature.settings.SettingsNavBridge
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

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
    val scope = rememberCoroutineScope()
    var provider by remember {
        mutableStateOf(
            when (prefs.getString(PROVIDER, "auto") ?: "auto") {
                "google" -> "google_cloud"
                else -> prefs.getString(PROVIDER, "auto") ?: "auto"
            }
        )
    }
    var endpoint by remember { mutableStateOf(prefs.getString(ENDPOINT, "https://translate.terraprint.co/translate") ?: "") }
    var apiKey by remember { mutableStateOf(prefs.getString(API_KEY, "") ?: "") }
    var source by remember { mutableStateOf(prefs.getString(SOURCE, "AUTO") ?: "AUTO") }
    var target by remember { mutableStateOf(prefs.getString(TARGET, "RU") ?: "RU") }
    var model by remember { mutableStateOf(prefs.getString(MODEL, "gpt-4o-mini") ?: "gpt-4o-mini") }
    var status by remember { mutableStateOf("Бесплатный автоматический перевод готов") }
    var testText by remember { mutableStateOf("The door is locked. Find the key.") }
    var testResult by remember { mutableStateOf("") }
    var testing by remember { mutableStateOf(false) }

    fun save() {
        prefs.edit().putString(PROVIDER, provider).putString(ENDPOINT, endpoint.trim())
            .putString(API_KEY, apiKey.trim()).putString(SOURCE, source.trim().uppercase())
            .putString(TARGET, target.trim().uppercase()).putString(MODEL, model.trim()).apply()
        status = "Настройки сохранены"
    }

    val providers = listOf(
        "auto" to "Авто: Google → Yandex → MyMemory → Apertium (без ключей)",
        "google_free" to "Google Translate (без ключа, неофициальный endpoint)",
        "mymemory" to "MyMemory (без ключа, лимит запросов)",
        "apertium" to "Apertium (без ключа, ограниченные языковые пары)",
        "yandex_free" to "Yandex Translate Web (без ключа, экспериментально)",
        "yandex" to "Yandex Translate API (нужен ключ)",
        "google_cloud" to "Google Cloud Translation (нужен ключ)",
        "deepl" to "DeepL API (нужен ключ)",
        "libre" to "LibreTranslate (сервер может требовать ключ)",
        "openai" to "OpenAI-compatible API (нужен ключ)"
    )
    val needsCredentials = provider in setOf("yandex", "google_cloud", "deepl", "libre", "openai")

    Column(
        Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(18.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        Text("Игровой переводчик", style = MaterialTheme.typography.headlineMedium)
        Text(
            "Перевод распознанных игровых фраз. По умолчанию используется бесплатный режим без регистрации, API-ключей и ручной настройки endpoint.",
            style = MaterialTheme.typography.bodyMedium
        )

        Card {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("Запуск в игре", style = MaterialTheme.typography.titleMedium)
                Text(
                    "Запускай и останавливай переводчик в панели FPS / Frame Generation во время Wayland-сессии. Кадры поступают из Turnip/Vulkan-композитора; Android MediaProjection не используется.",
                    style = MaterialTheme.typography.bodySmall
                )
                Text(status, style = MaterialTheme.typography.bodyMedium)
            }
        }

        Card {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("Сервис перевода", style = MaterialTheme.typography.titleMedium)
                providers.forEach { (id, label) ->
                    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                        Text(label, modifier = Modifier.weight(1f), style = MaterialTheme.typography.bodyMedium)
                        RadioButton(selected = provider == id, onClick = {
                            provider = id
                            endpoint = when (id) {
                                "deepl" -> "https://api-free.deepl.com/v2/translate"
                                "libre" -> "https://translate.terraprint.co/translate"
                                "google_cloud" -> "https://translation.googleapis.com/language/translate/v2"
                                "yandex" -> "https://translate.yandex.net/api/v1/tr.json/translate"
                                else -> endpoint
                            }
                        })
                    }
                }
                if (needsCredentials) {
                    if (provider in setOf("libre", "openai")) {
                        OutlinedTextField(endpoint, { endpoint = it }, label = { Text("HTTPS API endpoint") }, modifier = Modifier.fillMaxWidth(), singleLine = true)
                    }
                    OutlinedTextField(apiKey, { apiKey = it }, label = { Text("API key") }, modifier = Modifier.fillMaxWidth(), singleLine = true)
                    if (provider == "openai") {
                        OutlinedTextField(model, { model = it }, label = { Text("AI model") }, modifier = Modifier.fillMaxWidth(), singleLine = true)
                    }
                } else {
                    Text("Ключи и endpoint не нужны. В режиме «Авто» приложение само переключится на следующий сервис при ошибке.", style = MaterialTheme.typography.bodySmall)
                }
            }
        }

        Card {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("Проверка перевода", style = MaterialTheme.typography.titleMedium)
                Text("Отправляет тестовую фразу выбранному сервису. Работает отдельно от захвата экрана.", style = MaterialTheme.typography.bodySmall)
                OutlinedTextField(
                    value = testText,
                    onValueChange = { testText = it.take(1000) },
                    label = { Text("Текст для перевода") },
                    modifier = Modifier.fillMaxWidth(),
                    minLines = 2
                )
                Button(
                    enabled = !testing && testText.isNotBlank(),
                    onClick = {
                        save()
                        testing = true
                        testResult = "Запрос отправлен…"
                        scope.launch {
                            val result = runCatching {
                                withContext(Dispatchers.IO) {
                                    TranslationProvider.translate(context, testText.trim())
                                }
                            }
                            testResult = result.fold(
                                onSuccess = { "Перевод ($target):\\n$it" },
                                onFailure = { "Ошибка: ${it.message ?: "неизвестная ошибка"}" }
                            )
                            testing = false
                        }
                    },
                    modifier = Modifier.fillMaxWidth()
                ) { Text(if (testing) "ПЕРЕВОД…" else "ПРОВЕРИТЬ ПЕРЕВОД") }
                if (testResult.isNotBlank()) {
                    Surface(
                        modifier = Modifier.fillMaxWidth(),
                        color = MaterialTheme.colorScheme.surfaceVariant,
                        shape = MaterialTheme.shapes.medium
                    ) {
                        Text(testResult, modifier = Modifier.padding(12.dp), style = MaterialTheme.typography.bodyMedium)
                    }
                }
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

        Text(
            "Примечание: Google Translate без ключа использует неофициальный веб-endpoint и может ограничивать запросы. MyMemory имеет дневные лимиты; Apertium поддерживает не все языковые пары. Yandex Web работает без ключа через неофициальный виджет и может блокировать запросы CAPTCHA; официальный Yandex API требует ключ. Распознанный текст отправляется выбранному внешнему сервису перевода.",
            style = MaterialTheme.typography.bodySmall
        )
    }
}
