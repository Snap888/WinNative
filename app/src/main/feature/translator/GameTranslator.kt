package com.winlator.cmod.feature.translator

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Rect
import android.graphics.Typeface
import android.text.Layout
import android.text.StaticLayout
import android.text.TextPaint
import android.text.TextUtils
import com.google.mlkit.vision.common.InputImage
import com.google.mlkit.vision.text.TextRecognizer
import com.google.mlkit.vision.text.TextRecognition
import com.google.mlkit.vision.text.Text as MlText
import com.google.mlkit.vision.text.latin.TextRecognizerOptions
import com.google.mlkit.vision.text.japanese.JapaneseTextRecognizerOptions
import com.google.mlkit.vision.text.chinese.ChineseTextRecognizerOptions
import com.google.mlkit.vision.text.korean.KoreanTextRecognizerOptions
import com.winlator.cmod.runtime.display.wayland.WaylandCompositor
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.material3.Button
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
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
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import kotlin.math.max
import kotlin.math.min

enum class TranslatorDisplayMode(val title: String, val description: String) {
    ORIGINAL_AND_TRANSLATION("A • Оригинал + перевод", "Перевод под исходной строкой"),
    REPLACE_TEXT("B • Замена текста", "Перевод на месте оригинала • матовое стекло"),
    BESIDE_ORIGINAL("C • Рядом с оригиналом", "Перевод справа от исходной строки")
}

/** Turnip scene readback -> multi-script ML Kit OCR -> keyless translation -> Vulkan overlay. */
object GameTranslator {
    private const val PREFS = "wintranslator"
    private const val MODE_KEY = "display_mode"
    private const val MAX_LINES = 18
    private val worker = Executors.newSingleThreadExecutor { r -> Thread(r, "GameTranslator-OCR") }
    private val translationWorker = Executors.newFixedThreadPool(2) { r -> Thread(r, "GameTranslator-Network") }
    private val translationsInFlight = ConcurrentHashMap.newKeySet<String>()
    private val busy = AtomicBoolean(false)
    private val enabledState = mutableStateOf(false)
    private val statusState = mutableStateOf("Выключен")
    private val displayModeState = mutableStateOf(TranslatorDisplayMode.ORIGINAL_AND_TRANSLATION)
    @Volatile private var appContext: Context? = null
    @Volatile private var lastSource = ""
    @Volatile private var lastTranslationAt = 0L
    @Volatile private var lastTranslationFailureAt = 0L
    @Volatile private var lastVisibleTextAt = 0L
    @Volatile private var candidateText = ""
    @Volatile private var candidateHits = 0
    @Volatile private var lastRenderedGeometry = ""
    @Volatile private var lastTranslatedText = ""

    private val latin by lazy { TextRecognition.getClient(TextRecognizerOptions.DEFAULT_OPTIONS) }
    private val japanese by lazy { TextRecognition.getClient(JapaneseTextRecognizerOptions.Builder().build()) }
    private val chinese by lazy { TextRecognition.getClient(ChineseTextRecognizerOptions.Builder().build()) }
    private val korean by lazy { TextRecognition.getClient(KoreanTextRecognizerOptions.Builder().build()) }

    private data class OcrLine(val text: String, val bounds: Rect)

    val enabled: Boolean get() = enabledState.value
    val status: String get() = statusState.value
    val displayMode: TranslatorDisplayMode get() = displayModeState.value

    fun setDisplayMode(context: Context, mode: TranslatorDisplayMode) {
        displayModeState.value = mode
        context.applicationContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .edit().putString(MODE_KEY, mode.name).apply()
        lastSource = "" // Re-render the next live OCR frame using the newly selected layout.
        WaylandCompositor.nativeSetTranslatorOverlay(byteArrayOf(), 0, 0)
        statusState.value = "Режим отображения: ${mode.title}"
    }

    fun start(context: Context) {
        if (!WaylandCompositor.nativeIsRunning()) {
            statusState.value = "Запусти игру в Wayland-сессии перед включением перевода"
            return
        }
        appContext = context.applicationContext
        val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        displayModeState.value = runCatching {
            TranslatorDisplayMode.valueOf(prefs.getString(MODE_KEY, null) ?: "")
        }.getOrDefault(TranslatorDisplayMode.ORIGINAL_AND_TRANSLATION)
        lastSource = ""
        candidateText = ""
        candidateHits = 0
        lastVisibleTextAt = 0L
        lastRenderedGeometry = ""
        lastTranslatedText = ""
        enabledState.value = true
        statusState.value = "Turnip: захват сцены включён, ожидание текста"
        WaylandCompositor.nativeSetTranslatorOverlay(byteArrayOf(), 0, 0)
        WaylandCompositor.nativeSetTranslatorEnabled(true)
    }

    fun stop() {
        enabledState.value = false
        WaylandCompositor.nativeSetTranslatorEnabled(false)
        WaylandCompositor.nativeSetTranslatorOverlay(byteArrayOf(), 0, 0)
        statusState.value = "Выключен"
        lastSource = ""
        candidateText = ""
        candidateHits = 0
        lastTranslatedText = ""
        lastRenderedGeometry = ""
        busy.set(false)
    }

    @JvmStatic
    fun onNativeFrame(width: Int, height: Int, rgba: ByteArray) {
        val context = appContext ?: return
        if (!enabledState.value || width <= 0 || height <= 0 || rgba.size < width * height * 4) return
        if (!busy.compareAndSet(false, true)) return
        worker.execute {
            var bitmap: Bitmap? = null
            try {
                val frameBitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
                bitmap = frameBitmap
                val pixels = IntArray(width * height)
                var p = 0
                for (i in pixels.indices) {
                    val r = rgba[p++].toInt() and 0xff
                    val g = rgba[p++].toInt() and 0xff
                    val b = rgba[p++].toInt() and 0xff
                    val a = rgba[p++].toInt() and 0xff
                    pixels[i] = Color.argb(a, r, g, b)
                }
                frameBitmap.setPixels(pixels, 0, width, 0, 0, width, height)
                val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
                val source = (prefs.getString("source", "AUTO") ?: "AUTO").uppercase()
                recognizeAll(InputImage.fromBitmap(frameBitmap, 0), source, width, height,
                    onSuccess = success@{ lines ->
                        var bitmapTransferred = false
                        try {
                            // Keep identical labels in different HUD locations: they are distinct
                            // visible strings and must retain their own OCR anchors.
                            val clean = lines.take(MAX_LINES)
                            val text = clean.joinToString("\n") { it.text }.take(1400)
                            val normalized = text.lowercase().replace(Regex("\\s+"), " ").trim()
                            val now = System.currentTimeMillis()
                            if (normalized.isBlank()) {
                                // A single missed OCR frame is not proof that the game text vanished.
                                // Keep the last confirmed overlay briefly to mask recognizer jitter.
                                if (now - lastVisibleTextAt > 1400L) {
                                    lastSource = ""
                                    lastTranslatedText = ""
                                    lastRenderedGeometry = ""
                                    candidateText = ""
                                    candidateHits = 0
                                    WaylandCompositor.nativeSetTranslatorOverlay(byteArrayOf(), 0, 0)
                                    statusState.value = "Turnip: текста в кадре нет"
                                } else {
                                    statusState.value = "OCR: кратковременный пропуск, сохраняю перевод"
                                }
                            } else {
                                lastVisibleTextAt = now
                                // Confirm a changed dialogue twice before replacing the current
                                // overlay. This filters one-frame OCR substitutions during animation.
                                if (normalized != lastSource && lastSource.isNotEmpty()) {
                                    if (candidateText == normalized) candidateHits++ else {
                                        candidateText = normalized
                                        candidateHits = 1
                                    }
                                    if (candidateHits < 2) {
                                        statusState.value = "OCR: подтверждаю новую строку…"
                                        return@success
                                    }
                                } else {
                                    candidateText = ""
                                    candidateHits = 0
                                }

                                val geometry = clean.joinToString("|") {
                                    "${it.bounds.left / 16},${it.bounds.top / 16},${it.bounds.right / 16},${it.bounds.bottom / 16}"
                                }
                                if (normalized == lastSource) {
                                    // Same dialogue: do not clear/retranslate. Reposition only when
                                    // the OCR anchor moved by a meaningful amount.
                                    if (geometry != lastRenderedGeometry && lastTranslatedText.isNotBlank()) {
                                        drawModeOverlay(frameBitmap, width, height, clean,
                                            lastTranslatedText, displayModeState.value)
                                        lastRenderedGeometry = geometry
                                    } else if (lastTranslatedText.isBlank() &&
                                        now - lastTranslationFailureAt >= 2500L) {
                                        lastRenderedGeometry = geometry
                                        bitmapTransferred = translateAsync(context, frameBitmap, width, height,
                                            clean, text, normalized, geometry)
                                    }
                                } else {
                                    lastSource = normalized
                                    // Keep the submitted Vulkan overlay visible, but do not reuse
                                    // its text as if it belonged to the newly detected dialogue.
                                    lastTranslatedText = ""
                                    lastRenderedGeometry = geometry
                                    lastTranslationFailureAt = 0L
                                    statusState.value = "OCR: ${clean.size} строк, перевожу…"
                                    bitmapTransferred = translateAsync(context, frameBitmap, width, height,
                                        clean, text, normalized, geometry)
                                }
                            }
                        } finally {
                            if (!bitmapTransferred) bitmap?.recycle()
                            busy.set(false)
                        }
                    },
                    onFailure = { error ->
                        bitmap?.recycle()
                        if (System.currentTimeMillis() - lastVisibleTextAt > 1400L) {
                            WaylandCompositor.nativeSetTranslatorOverlay(byteArrayOf(), 0, 0)
                            lastSource = ""
                            lastTranslatedText = ""
                            lastRenderedGeometry = ""
                        }
                        statusState.value = "OCR: временная ошибка распознавания"
                        busy.set(false)
                    })
            } catch (error: Throwable) {
                bitmap?.recycle()
                if (System.currentTimeMillis() - lastVisibleTextAt > 1400L) {
                    WaylandCompositor.nativeSetTranslatorOverlay(byteArrayOf(), 0, 0)
                    lastSource = ""
                    lastTranslatedText = ""
                    lastRenderedGeometry = ""
                }
                statusState.value = "Ошибка обработки кадра: ${error.message ?: "неизвестно"}"
                busy.set(false)
            }
        }
    }

    /** Network work is isolated from OCR so new frames can be processed while a provider responds. */
    private fun translateAsync(context: Context, frameBitmap: Bitmap, width: Int, height: Int,
        lines: List<OcrLine>, text: String, normalized: String, geometry: String): Boolean {
        if (!translationsInFlight.add(normalized)) return false
        translationWorker.execute {
            try {
                val result = runCatching { TranslationProvider.translate(context, text) }
                val translated = result.getOrNull().orEmpty()
                lastTranslationAt = System.currentTimeMillis()
                if (!enabledState.value || lastSource != normalized || lastRenderedGeometry != geometry) return@execute
                if (result.isFailure || translated.isBlank()) {
                    lastTranslationFailureAt = System.currentTimeMillis()
                    statusState.value = "Перевод временно недоступен; повтор через 2,5 секунды"
                    return@execute
                }
                lastTranslationFailureAt = 0L
                lastTranslatedText = translated
                drawModeOverlay(frameBitmap, width, height, lines, translated, displayModeState.value)
                lastRenderedGeometry = geometry
                statusState.value = "Перевод готов • ${displayModeState.value.title}"
            } finally {
                translationsInFlight.remove(normalized)
                if (!frameBitmap.isRecycled) frameBitmap.recycle()
            }
        }
        return true
    }

    /**
     * AUTO deliberately runs every installed script recognizer. Results are merged by geometry/text,
     * so Latin HUD labels no longer prevent Japanese/Chinese/Korean dialogue from being detected.
     */
    private fun recognizeAll(
        input: InputImage,
        source: String,
        width: Int,
        height: Int,
        onSuccess: (List<OcrLine>) -> Unit,
        onFailure: (Throwable) -> Unit
    ) {
        val recognizers: List<TextRecognizer> = when (source) {
            "JA" -> listOf(japanese)
            "ZH", "ZH-CN", "ZH-TW" -> listOf(chinese)
            "KO" -> listOf(korean)
            "AUTO" -> listOf(latin, japanese, chinese, korean)
            else -> listOf(latin)
        }
        val collected = java.util.Collections.synchronizedList(mutableListOf<OcrLine>())
        val remaining = AtomicInteger(recognizers.size)
        val errorLock = Any()
        var lastError: Throwable? = null

        fun finishOne() {
            if (remaining.decrementAndGet() != 0) return
            val snapshot = synchronized(collected) { collected.toList() }
            val failure = synchronized(errorLock) { lastError }
            if (snapshot.isEmpty() && failure != null) onFailure(failure)
            else onSuccess(snapshot.sortedWith(compareBy<OcrLine> { it.bounds.top }.thenBy { it.bounds.left }))
        }

        // Start every required script recognizer immediately instead of waiting for four
        // sequential ML Kit passes. Listener callbacks are serialized on the OCR worker.
        recognizers.forEach { recognizer ->
            try {
                recognizer.process(input)
                    .addOnSuccessListener(worker) { result: MlText ->
                        synchronized(collected) {
                            result.textBlocks.forEach { block ->
                                block.lines.forEach { line ->
                                    val text = line.text.trim()
                                    val box = line.boundingBox
                                    if (text.length >= 2 && box != null) {
                                        val clipped = Rect(
                                            box.left.coerceIn(0, width - 1),
                                            box.top.coerceIn(0, height - 1),
                                            box.right.coerceIn(1, width),
                                            box.bottom.coerceIn(1, height)
                                        )
                                        if (clipped.width() > 0 && clipped.height() > 0) {
                                            val duplicate = collected.any {
                                                it.text.equals(text, ignoreCase = true) &&
                                                    Rect.intersects(it.bounds, clipped)
                                            }
                                            if (!duplicate) collected.add(OcrLine(text, clipped))
                                        }
                                    }
                                }
                            }
                        }
                        finishOne()
                    }
                    .addOnFailureListener(worker) { error ->
                        synchronized(errorLock) { lastError = error }
                        finishOne()
                    }
            } catch (error: Throwable) {
                synchronized(errorLock) { lastError = error }
                finishOne()
            }
        }
    }

    private fun drawModeOverlay(
        sourceFrame: Bitmap,
        sceneWidth: Int,
        sceneHeight: Int,
        lines: List<OcrLine>,
        translated: String,
        mode: TranslatorDisplayMode
    ) {
        // A full-scene transparent layer keeps each label anchored to its OCR rectangle.
        // Vulkan alpha blending is enabled for this draw; B paints only the original text boxes.
        val bitmap = Bitmap.createBitmap(sceneWidth, sceneHeight, Bitmap.Config.ARGB_8888)
        val canvas = Canvas(bitmap)
        val translations = translated.split('\n').map { it.trim() }.filter { it.isNotEmpty() }
        val lineMappingValid = translations.size == lines.size
        if (!lineMappingValid) {
            // Never show an unanchored translation: it could describe text that is no longer
            // visible or be paired with the wrong dialogue line.
            bitmap.recycle()
            WaylandCompositor.nativeSetTranslatorOverlay(byteArrayOf(), 0, 0)
            statusState.value = "Перевод скрыт: сервис изменил структуру строк"
            return
        }
        val softenedFrame = Bitmap.createScaledBitmap(
            sourceFrame,
            max(1, sceneWidth / 8),
            max(1, sceneHeight / 8),
            true
        )
        val textPaint = TextPaint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.WHITE
            textSize = (sceneWidth / 58f).coerceIn(15f, 30f)
            typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
            setShadowLayer(3f, 1f, 2f, Color.BLACK)
        }
        val stroke = TextPaint(textPaint).apply {
            color = Color.BLACK
            style = Paint.Style.STROKE
            strokeWidth = max(2f, textPaint.textSize / 8f)
            clearShadowLayer()
        }

        run {
            lines.forEachIndexed { index, line ->
                val box = line.bounds
                val translatedLine = translations[index]
                val margin = max(5, box.height() / 7)
                when (mode) {
                    TranslatorDisplayMode.ORIGINAL_AND_TRANSLATION -> {
                        // Keep the original untouched. Use the wider free screen area, but stop
                        // before the next OCR line to avoid writing over adjacent dialogue.
                        val nextTop = lines.filter { it.bounds.top > box.top }
                            .minOfOrNull { it.bounds.top } ?: sceneHeight
                        val belowY = box.bottom + margin
                        val belowSpace = min(sceneHeight, nextTop) - belowY
                        val aboveBottom = box.top - margin
                        val aboveSpace = aboveBottom
                        val y: Int
                        val safeHeight: Int
                        if (belowSpace >= 24) {
                            y = belowY
                            safeHeight = belowSpace
                        } else if (aboveSpace >= 24) {
                            safeHeight = min(aboveSpace, max(box.height(), 28))
                            y = aboveBottom - safeHeight
                        } else {
                            // Do not cover the original or neighboring HUD when no safe slot exists.
                            return@forEachIndexed
                        }
                        val width = (sceneWidth - box.left - 8).coerceAtLeast(box.width())
                        drawFitted(canvas, translatedLine, box.left, y, width, safeHeight, textPaint, stroke)
                    }
                    TranslatorDisplayMode.REPLACE_TEXT -> {
                        // Frosted-glass patch: downsampled live scene provides a soft blur, then
                        // a translucent cool tint and fine highlight create a glass-like surface.
                        val padX = max(3, box.height() / 10)
                        val padY = max(2, box.height() / 12)
                        val nextTop = lines.filter { it !== line && it.bounds.top >= box.bottom }
                            .minOfOrNull { it.bounds.top } ?: sceneHeight
                        val glassBottom = min(nextTop - 2,
                            box.top + max(box.height() * 2, textPaint.textSize.toInt() * 2))
.coerceAtLeast(box.bottom)
                        val rect = Rect(
                            max(0, box.left - padX), max(0, box.top - padY),
                            min(sceneWidth, box.right + padX), min(sceneHeight, glassBottom)
                        )
                        drawFrostedGlass(canvas, softenedFrame, rect, sceneWidth, sceneHeight)
                        val inner = max(3, box.height() / 12)
                        drawFitted(canvas, translatedLine, rect.left + inner, rect.top + inner,
                            (rect.width() - inner * 2).coerceAtLeast(24),
                            (rect.height() - inner * 2).coerceAtLeast(18), textPaint, stroke)
                    }
                    TranslatorDisplayMode.BESIDE_ORIGINAL -> {
                        val rightSpace = sceneWidth - box.right - margin
                        val rightRect = Rect(box.right + margin, box.top,
                            sceneWidth - 6, min(sceneHeight, box.bottom + max(box.height() * 2, 48)))
                        val overlapsRight = lines.any { other ->
                            other !== line && Rect.intersects(rightRect, other.bounds)
                        }
                        if (rightSpace >= max(120, box.width() / 2) && !overlapsRight) {
                            drawFitted(canvas, translatedLine, rightRect.left, rightRect.top,
                                rightRect.width(), rightRect.height(), textPaint, stroke)
                        } else {
                            val nextTop = lines.filter { it.bounds.top > box.top }
                                .minOfOrNull { it.bounds.top } ?: sceneHeight
                            val belowY = box.bottom + margin
                            val available = (nextTop - belowY).coerceAtLeast(0)
                            if (available >= 24) {
                                drawFitted(canvas, translatedLine, box.left, belowY,
                                    (sceneWidth - box.left - 8).coerceAtLeast(box.width()),
                                    available, textPaint, stroke)
                            }
                        }
                    }
                }
            }
        }
        softenedFrame.recycle()

        val argb = IntArray(sceneWidth * sceneHeight)
        bitmap.getPixels(argb, 0, sceneWidth, 0, 0, sceneWidth, sceneHeight)
        val rgba = ByteArray(argb.size * 4)
        var out = 0
        for (pixel in argb) {
            rgba[out++] = ((pixel shr 16) and 0xff).toByte()
            rgba[out++] = ((pixel shr 8) and 0xff).toByte()
            rgba[out++] = (pixel and 0xff).toByte()
            rgba[out++] = ((pixel ushr 24) and 0xff).toByte()
        }
        bitmap.recycle()
        WaylandCompositor.nativeSetTranslatorOverlay(rgba, sceneWidth, sceneHeight)
    }

    private fun drawFrostedGlass(
        canvas: Canvas,
        blurredScene: Bitmap,
        rect: Rect,
        sceneWidth: Int,
        sceneHeight: Int
    ) {
        if (rect.width() <= 0 || rect.height() <= 0) return
        val radius = (rect.height() * 0.16f).coerceIn(4f, 12f)
        val path = android.graphics.Path().apply {
            addRoundRect(rect.left.toFloat(), rect.top.toFloat(), rect.right.toFloat(),
                rect.bottom.toFloat(), radius, radius, android.graphics.Path.Direction.CW)
        }
        val src = Rect(
            (rect.left.toFloat() / sceneWidth * blurredScene.width).toInt().coerceIn(0, blurredScene.width - 1),
            (rect.top.toFloat() / sceneHeight * blurredScene.height).toInt().coerceIn(0, blurredScene.height - 1),
            (rect.right.toFloat() / sceneWidth * blurredScene.width).toInt().coerceIn(1, blurredScene.width),
            (rect.bottom.toFloat() / sceneHeight * blurredScene.height).toInt().coerceIn(1, blurredScene.height)
        )
        canvas.save()
        canvas.clipPath(path)
        canvas.drawBitmap(blurredScene, src, android.graphics.RectF(rect),
            Paint(Paint.ANTI_ALIAS_FLAG or Paint.FILTER_BITMAP_FLAG).apply { alpha = 190 })
        canvas.drawRoundRect(android.graphics.RectF(rect), radius, radius,
            Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0x705b7188; style = Paint.Style.FILL })
        canvas.restore()
        canvas.drawRoundRect(android.graphics.RectF(rect), radius, radius,
            Paint(Paint.ANTI_ALIAS_FLAG).apply {
                color = 0x9feaf4ff.toInt()
                style = Paint.Style.STROKE
                strokeWidth = max(1f, rect.height() / 36f)
            })
        canvas.drawLine(rect.left + radius, rect.top + 1f, rect.right - radius, rect.top + 1f,
            Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0x80ffffff.toInt(); strokeWidth = 1f })
    }

    private fun drawTextBlock(
        canvas: Canvas, text: String, width: Int, height: Int,
        paint: TextPaint, stroke: TextPaint
    ) {
        val panelWidth = min(width - 32, 900).coerceAtLeast(160)
        val layout = StaticLayout.Builder.obtain(text, 0, text.length, paint, panelWidth - 28)
            .setAlignment(Layout.Alignment.ALIGN_NORMAL).setIncludePad(false).setMaxLines(10)
            .setEllipsize(TextUtils.TruncateAt.END).build()
        val x = 16f
        val y = (height - layout.height - 36).coerceAtLeast(8).toFloat()
        canvas.drawRoundRect(x, y, x + panelWidth, y + layout.height + 20, 8f, 8f,
            Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xb8202630.toInt() })
        canvas.save()
        canvas.translate(x + 14, y + 10)
        layout.draw(canvas)
        canvas.restore()
    }

    private fun drawFitted(
        canvas: Canvas, text: String, x: Int, y: Int, maxWidth: Int, maxHeight: Int,
        paint: TextPaint, stroke: TextPaint
    ) {
        if (maxWidth <= 8 || maxHeight <= 8 || text.isBlank()) return
        var size = paint.textSize
        var layout: StaticLayout
        do {
            paint.textSize = size
            stroke.textSize = size
            layout = StaticLayout.Builder.obtain(text, 0, text.length, paint, maxWidth)
                .setAlignment(Layout.Alignment.ALIGN_NORMAL).setIncludePad(false).setMaxLines(3)
                .setEllipsize(TextUtils.TruncateAt.END).build()
            if (layout.height <= maxHeight || size <= 11f) break
            size -= 2f
        } while (size > 9f)
        canvas.save()
        canvas.clipRect(x, y, min(canvas.width, x + maxWidth), min(canvas.height, y + maxHeight))
        canvas.translate(x.toFloat(), y.toFloat())
        val strokeLayout = StaticLayout.Builder.obtain(text, 0, text.length, stroke, maxWidth)
            .setAlignment(Layout.Alignment.ALIGN_NORMAL).setIncludePad(false).setMaxLines(3)
            .setEllipsize(TextUtils.TruncateAt.END).build()
        strokeLayout.draw(canvas)
        layout.draw(canvas)
        canvas.restore()
        paint.textSize = (canvas.width / 58f).coerceIn(15f, 30f)
        stroke.textSize = paint.textSize
    }
}

@Composable
internal fun GameTranslatorPanel(paneScale: Float) {
    val context = LocalContext.current
    val enabled = GameTranslator.enabled
    val status = GameTranslator.status
    val selectedMode = GameTranslator.displayMode
    var menuExpanded by remember { mutableStateOf(false) }
    Column(verticalArrangement = Arrangement.spacedBy((6f * paneScale).dp)) {
        Text("Игровой переводчик", color = MaterialTheme.colorScheme.onSurface, fontSize = (13f * paneScale).sp)
        Button(
            onClick = { if (enabled) GameTranslator.stop() else GameTranslator.start(context) },
            modifier = Modifier.fillMaxWidth()
        ) { Text(if (enabled) "ОСТАНОВИТЬ ПЕРЕВОД" else "ВКЛЮЧИТЬ ПЕРЕВОД В ИГРЕ") }
        Row {
            Button(onClick = { menuExpanded = true }, modifier = Modifier.fillMaxWidth()) {
                Text("Режим: ${selectedMode.title}")
            }
            DropdownMenu(expanded = menuExpanded, onDismissRequest = { menuExpanded = false }) {
                TranslatorDisplayMode.values().forEach { mode ->
                    DropdownMenuItem(
                        text = { Text(mode.title) },
                        onClick = {
                            GameTranslator.setDisplayMode(context, mode)
                            menuExpanded = false
                        }
                    )
                }
            }
        }
        Text(selectedMode.description, color = MaterialTheme.colorScheme.onSurfaceVariant,
            fontSize = (10f * paneScale).sp)
        Text("Turnip/Vulkan → OCR → перевод с привязкой к исходным координатам → Vulkan overlay",
            color = MaterialTheme.colorScheme.onSurfaceVariant, fontSize = (10f * paneScale).sp)
        Text(status, color = MaterialTheme.colorScheme.onSurfaceVariant, fontSize = (11f * paneScale).sp)
    }
}
