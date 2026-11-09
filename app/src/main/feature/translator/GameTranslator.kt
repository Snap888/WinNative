package com.winlator.cmod.feature.translator

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.text.Layout
import android.text.StaticLayout
import android.text.TextPaint
import android.graphics.Typeface
import com.google.android.gms.tasks.Task
import com.google.mlkit.vision.common.InputImage
import com.google.mlkit.vision.text.TextRecognizer
import com.google.mlkit.vision.text.TextRecognition
import com.google.mlkit.vision.text.latin.TextRecognizerOptions
import com.google.mlkit.vision.text.japanese.JapaneseTextRecognizerOptions
import com.google.mlkit.vision.text.chinese.ChineseTextRecognizerOptions
import com.google.mlkit.vision.text.korean.KoreanTextRecognizerOptions
import com.winlator.cmod.runtime.display.wayland.WaylandCompositor
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean

/**
 * End-to-end in-game translation for the embedded Wayland compositor:
 * Turnip scene readback -> on-device ML Kit OCR -> keyless translation providers -> Vulkan draw.
 * The compositor sends frames at a low cadence; all OCR/network work runs on this worker.
 */
object GameTranslator {
    private const val PREFS = "wintranslator"
    private val worker = Executors.newSingleThreadExecutor { r -> Thread(r, "GameTranslator-OCR") }
    private val busy = AtomicBoolean(false)
    private val enabledState = mutableStateOf(false)
    private val statusState = mutableStateOf("Выключен")
    @Volatile private var appContext: Context? = null
    @Volatile private var lastSource = ""
    @Volatile private var lastTranslationAt = 0L

    private val latin by lazy { TextRecognition.getClient(TextRecognizerOptions.DEFAULT_OPTIONS) }
    private val japanese by lazy { TextRecognition.getClient(JapaneseTextRecognizerOptions.Builder().build()) }
    private val chinese by lazy { TextRecognition.getClient(ChineseTextRecognizerOptions.Builder().build()) }
    private val korean by lazy { TextRecognition.getClient(KoreanTextRecognizerOptions.Builder().build()) }

    val enabled: Boolean get() = enabledState.value
    val status: String get() = statusState.value

    fun start(context: Context) {
        appContext = context.applicationContext
        lastSource = ""
        enabledState.value = true
        statusState.value = "Turnip: включён захват сцены, ожидание текста"
        WaylandCompositor.nativeSetTranslatorEnabled(true)
    }

    fun stop() {
        enabledState.value = false
        WaylandCompositor.nativeSetTranslatorEnabled(false)
        WaylandCompositor.nativeSetTranslatorOverlay(byteArrayOf(), 0, 0)
        statusState.value = "Выключен"
        busy.set(false)
    }

    @JvmStatic
    fun onNativeFrame(width: Int, height: Int, rgba: ByteArray) {
        val context = appContext ?: return
        if (!enabledState.value || width <= 0 || height <= 0 || rgba.size < width * height * 4) return
        if (!busy.compareAndSet(false, true)) return
        worker.execute {
            try {
                val bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
                val pixels = IntArray(width * height)
                var p = 0
                for (i in pixels.indices) {
                    val r = rgba[p++].toInt() and 0xff
                    val g = rgba[p++].toInt() and 0xff
                    val b = rgba[p++].toInt() and 0xff
                    val a = rgba[p++].toInt() and 0xff
                    pixels[i] = Color.argb(a, r, g, b)
                }
                bitmap.setPixels(pixels, 0, width, 0, 0, width, height)
                val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
                val source = (prefs.getString("source", "AUTO") ?: "AUTO").uppercase()
                val recognizer = recognizerFor(source)
                recognizer.process(InputImage.fromBitmap(bitmap, 0))
                    .addOnSuccessListener(worker) { result ->
                        try {
                            val text = result.text.lines().map { it.trim() }
                                .filter { it.length >= 2 }
                                .distinct().joinToString("\n").take(1400)
                            val normalized = text.lowercase().replace(Regex("\\s+"), " ").trim()
                            if (text.isBlank()) {
                                statusState.value = "Turnip: кадр получен, ищу текст…"
                            } else if (normalized != lastSource) {
                                lastSource = normalized
                                statusState.value = "OCR: текст найден, перевожу…"
                                val translated = runCatching { TranslationProvider.translate(context, text) }
                                    .getOrElse { "Перевод недоступен: ${it.message ?: "ошибка сервиса"}" }
                                if (enabledState.value) {
                                    lastTranslationAt = System.currentTimeMillis()
                                    showTranslation(context, width, text, translated)
                                    statusState.value = if (translated.startsWith("Перевод недоступен:"))
                                        "OCR работает; ошибка сервиса перевода"
                                    else "Перевод готов • ${translated.take(72)}"
                                }
                            }
                        } finally {
                            bitmap.recycle()
                            busy.set(false)
                        }
                    }
                    .addOnFailureListener(worker) { error ->
                        bitmap.recycle()
                        statusState.value = "Ошибка OCR: ${error.message ?: "ML Kit"}"
                        busy.set(false)
                    }
            } catch (error: Throwable) {
                statusState.value = "Ошибка кадра: ${error.message ?: "неизвестно"}"
                busy.set(false)
            }
        }
    }

    private fun recognizerFor(source: String): TextRecognizer = when (source) {
        "JA" -> japanese
        "ZH", "ZH-CN", "ZH-TW" -> chinese
        "KO" -> korean
        else -> latin
    }

    private fun showTranslation(context: Context, sceneWidth: Int, original: String, translated: String) {
        val maxWidth = (sceneWidth - 48).coerceIn(240, 1200)
        val padding = 22f
        val originalPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = 0xffc5ced8.toInt()
            textSize = (sceneWidth / 70f).coerceIn(14f, 22f)
            typeface = Typeface.create("sans-serif", Typeface.NORMAL)
        }
        val translationPaint = TextPaint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.WHITE
            textSize = (sceneWidth / 48f).coerceIn(18f, 30f)
            typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
        }
        val shown = translated.take(500)
        val layout = StaticLayout.Builder.obtain(shown, 0, shown.length, translationPaint,
            (maxWidth - padding * 2).toInt())
            .setAlignment(Layout.Alignment.ALIGN_NORMAL)
            .setIncludePad(false)
            .setMaxLines(7)
            .setEllipsize(android.text.TextUtils.TruncateAt.END)
            .build()
        val originalLine = original.replace("\n", " ").take(110)
        val height = (padding * 2 + originalPaint.textSize + 10 + layout.height).toInt().coerceAtMost(440)
        val bitmap = Bitmap.createBitmap(maxWidth, height, Bitmap.Config.ARGB_8888)
        val canvas = Canvas(bitmap)
        canvas.drawColor(0xff101722.toInt())
        canvas.drawText(originalLine, padding, padding + originalPaint.textSize, originalPaint)
        canvas.save()
        canvas.translate(padding, padding + originalPaint.textSize + 12)
        layout.draw(canvas)
        canvas.restore()
        val argb = IntArray(maxWidth * height)
        bitmap.getPixels(argb, 0, maxWidth, 0, 0, maxWidth, height)
        val rgba = ByteArray(argb.size * 4)
        var o = 0
        for (pixel in argb) {
            rgba[o++] = ((pixel shr 16) and 0xff).toByte()
            rgba[o++] = ((pixel shr 8) and 0xff).toByte()
            rgba[o++] = (pixel and 0xff).toByte()
            rgba[o++] = ((pixel ushr 24) and 0xff).toByte()
        }
        bitmap.recycle()
        WaylandCompositor.nativeSetTranslatorOverlay(rgba, maxWidth, height)
    }
}

@Composable
internal fun GameTranslatorPanel(paneScale: Float) {
    val context = LocalContext.current
    val enabled = GameTranslator.enabled
    val status = GameTranslator.status
    Column(verticalArrangement = Arrangement.spacedBy((6f * paneScale).dp)) {
        Text(
            "Игровой переводчик",
            color = MaterialTheme.colorScheme.onSurface,
            fontSize = (13f * paneScale).sp,
        )
        Button(
            onClick = { if (enabled) GameTranslator.stop() else GameTranslator.start(context) },
            modifier = Modifier.fillMaxWidth(),
        ) {
            Text(if (enabled) "ОСТАНОВИТЬ ПЕРЕВОД" else "ВКЛЮЧИТЬ ПЕРЕВОД В ИГРЕ")
        }
        Text(
            "Turnip/Vulkan → ML Kit OCR → Google / Yandex / MyMemory → Vulkan overlay",
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            fontSize = (10f * paneScale).sp,
        )
        Text(status, color = MaterialTheme.colorScheme.onSurfaceVariant, fontSize = (11f * paneScale).sp)
    }
}
