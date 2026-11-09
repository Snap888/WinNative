package com.winlator.cmod.feature.translator

import android.content.Context
import org.json.JSONArray
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder

/**
 * Keyless-first translation backend for game text.
 *
 * Google Translate's gtx endpoint and Yandex's legacy web API are unofficial endpoints:
 * providers can throttle or change them without notice. Official Yandex/Google Cloud APIs
 * remain available as explicitly configured providers.
 */
internal object TranslationProvider {
    private const val PREFS = "wintranslator"

    fun translate(context: Context, text: String): String {
        require(text.isNotBlank()) { "Пустой текст" }
        val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        val provider = prefs.getString("provider", "auto") ?: "auto"
        val source = (prefs.getString("source", "AUTO") ?: "AUTO").lowercase()
        val target = (prefs.getString("target", "RU") ?: "RU").lowercase()
        val endpoint = prefs.getString("endpoint", "") ?: ""
        val key = prefs.getString("api_key", "") ?: ""
        val model = prefs.getString("model", "gpt-4o-mini") ?: "gpt-4o-mini"

        if (provider == "auto") {
            val attempts = mutableListOf<Pair<String, () -> String>>()
            attempts += "Google Translate" to { googleFree(text, source, target) }
            attempts += "MyMemory" to { myMemory(text, source, target) }
            if (source != "auto") attempts += "Apertium" to { apertium(text, source, target) }
            var last: Throwable? = null
            for ((name, action) in attempts) {
                try {
                    val result = action().trim()
                    if (result.isNotBlank()) return result
                    last = IllegalStateException("$name вернул пустой перевод")
                } catch (failure: Throwable) {
                    last = failure
                }
            }
            throw IllegalStateException("Не ответили бесплатные сервисы: ${last?.message ?: "нет результата"}", last)
        }

        return when (provider) {
            "google_free" -> googleFree(text, source, target)
            "mymemory" -> myMemory(text, source, target)
            "apertium" -> {
                require(source != "auto") { "Для Apertium укажи исходный язык (например EN)" }
                apertium(text, source, target)
            }
            "yandex" -> {
                require(key.isNotBlank()) { "Официальный Yandex API требует ключ. Для перевода без ключа выбери «Авто»." }
                yandex(text, source, target, key)
            }
            "google_cloud", "google" -> {
                require(key.isNotBlank()) { "Google Cloud Translation требует API key. Для бесплатного режима выбери «Авто»." }
                googleCloud(text, source, target, key)
            }
            "deepl" -> {
                require(key.isNotBlank()) { "DeepL требует API key" }
                deepL(text, source, target, key)
            }
            "libre" -> libre(text, source, target, endpoint, key)
            "openai" -> openAi(text, target, endpoint, key, model)
            else -> throw IllegalArgumentException("Неизвестный переводчик: $provider")
        }
    }

    private fun googleFree(text: String, source: String, target: String): String {
        val url = "https://translate.googleapis.com/translate_a/single?client=gtx" +
            "&sl=${enc(source)}&tl=${enc(target)}&dt=t&q=${enc(text)}"
        val root = JSONArray(request(url, "GET"))
        val segments = root.optJSONArray(0) ?: error("Некорректный ответ Google Translate")
        return buildString {
            for (i in 0 until segments.length()) {
                val segment = segments.optJSONArray(i)
                if (segment != null && !segment.isNull(0)) append(segment.optString(0))
            }
        }.also { if (it.isBlank()) error("Google Translate вернул пустой ответ") }
    }

    private fun myMemory(text: String, source: String, target: String): String {
        val pair = "${if (source == "auto") "autodetect" else source}|${target}"
        val url = "https://api.mymemory.translated.net/get?q=${enc(text)}&langpair=${enc(pair)}"
        val json = JSONObject(request(url, "GET"))
        val status = json.optInt("responseStatus", 200)
        if (status != 200) error(json.optString("responseDetails", "MyMemory HTTP $status"))
        return json.getJSONObject("responseData").getString("translatedText")
    }

    private fun apertium(text: String, source: String, target: String): String {
        val url = "https://apertium.org/apy/translate?q=${enc(text)}&langpair=${enc("$source|$target")}"
        val json = JSONObject(request(url, "GET"))
        return json.getJSONObject("responseData").getString("translatedText")
    }

    /** Yandex legacy Translate API. Requires a user-supplied key; no key is bundled. */
    private fun yandex(text: String, source: String, target: String, key: String): String {
        val params = "key=${enc(key)}&text=${enc(text)}&lang=${enc(if (source == "auto") target else "$source-$target")}"
        val json = JSONObject(request("https://translate.yandex.net/api/v1/tr.json/translate?$params", "GET"))
        val status = json.optInt("code", 200)
        if (status != 200) error(json.optString("message", "Yandex Translate HTTP $status"))
        return json.getJSONArray("text").optString(0)
    }

    private fun googleCloud(text: String, source: String, target: String, key: String): String {
        val json = JSONObject().put("q", text).put("target", target)
        if (source != "auto") json.put("source", source)
        val response = postJson(
            "https://translation.googleapis.com/language/translate/v2?key=${enc(key)}",
            json.toString()
        )
        return JSONObject(response).getJSONObject("data").getJSONArray("translations")
            .getJSONObject(0).getString("translatedText")
    }

    private fun deepL(text: String, source: String, target: String, key: String): String {
        val body = "text=${enc(text)}&target_lang=${enc(target.uppercase())}" +
            if (source == "auto") "" else "&source_lang=${enc(source.uppercase())}"
        val response = request(
            "https://api-free.deepl.com/v2/translate",
            "POST",
            body,
            mapOf("Authorization" to "DeepL-Auth-Key $key", "Content-Type" to "application/x-www-form-urlencoded; charset=utf-8")
        )
        return JSONObject(response).getJSONArray("translations").getJSONObject(0).getString("text")
    }

    private fun libre(text: String, source: String, target: String, endpoint: String, key: String): String {
        val url = endpoint.ifBlank { "https://translate.terraprint.co/translate" }
        require(url.startsWith("https://") || url.startsWith("http://10.") || url.startsWith("http://192.168.")) {
            "Укажи HTTPS endpoint LibreTranslate"
        }
        val body = JSONObject().put("q", text).put("source", source).put("target", target)
            .put("format", "text").also { if (key.isNotBlank()) it.put("api_key", key) }.toString()
        return JSONObject(postJson(url, body)).getString("translatedText")
    }

    private fun openAi(text: String, target: String, endpoint: String, key: String, model: String): String {
        require(key.isNotBlank()) { "OpenAI-compatible API требует ключ" }
        val url = endpoint.ifBlank { "https://api.openai.com/v1/chat/completions" }
        val prompt = "Translate game dialogue into language code $target. Preserve names, placeholders and line breaks. Return only the translation:\n$text"
        val body = JSONObject().put("model", model).put("temperature", 0.1)
            .put("messages", JSONArray().put(JSONObject().put("role", "user").put("content", prompt)))
        val response = JSONObject(postJson(url, body.toString(), mapOf("Authorization" to "Bearer $key")))
        return response.getJSONArray("choices").getJSONObject(0).getJSONObject("message").getString("content").trim()
    }

    private fun postJson(url: String, body: String, headers: Map<String, String> = emptyMap()): String =
        request(url, "POST", body, headers + ("Content-Type" to "application/json; charset=utf-8"))

    private fun request(
        address: String,
        method: String,
        body: String? = null,
        headers: Map<String, String> = emptyMap()
    ): String {
        val connection = URL(address).openConnection() as HttpURLConnection
        try {
            connection.requestMethod = method
            connection.connectTimeout = 8000
            connection.readTimeout = 12000
            connection.setRequestProperty("User-Agent", "Mozilla/5.0 (Linux; Android 15) AppleWebKit/537.36 Chrome/128 Mobile Safari/537.36")
            headers.forEach { (name, value) -> connection.setRequestProperty(name, value) }
            if (body != null) {
                connection.doOutput = true
                connection.outputStream.use { it.write(body.toByteArray(Charsets.UTF_8)) }
            }
            val code = connection.responseCode
            val stream = if (code in 200..299) connection.inputStream else connection.errorStream
            val response = stream?.bufferedReader(Charsets.UTF_8)?.use { it.readText() }.orEmpty()
            if (code !in 200..299) error("HTTP $code: ${response.take(180)}")
            return response
        } finally {
            connection.disconnect()
        }
    }

    private fun enc(value: String): String = URLEncoder.encode(value, "UTF-8")
}
