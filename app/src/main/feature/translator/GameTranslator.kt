package com.winlator.cmod.feature.translator

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.LinearGradient
import android.graphics.Shader
import android.graphics.Rect
import android.graphics.Typeface
import android.util.Log
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
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
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

enum class TranslatorPlaqueMode(val title: String) {
    GLASS("Жидкое стекло"),
    DENSE("Серый фон"),
    AUTO("Авто")
}

enum class TranslatorDisplayMode(val title: String, val description: String) {
    ORIGINAL_AND_TRANSLATION("A • Оригинал + перевод", "Перевод под исходной строкой"),
    REPLACE_TEXT("B • Замена текста", "Перевод на месте оригинала • матовое стекло"),
    BESIDE_ORIGINAL("C • Рядом с оригиналом", "Перевод справа от исходной строки")
}

/** Turnip scene readback -> multi-script ML Kit OCR -> keyless translation -> Vulkan overlay. */
object GameTranslator {
    private const val PREFS = "wintranslator"
    private const val MODE_KEY = "display_mode"
    private const val GLASS_ALPHA_KEY = "glass_alpha"
    private const val PLAQUE_MODE_KEY = "plaque_mode"
    private const val FONT_AUTO_KEY = "font_auto"
    private const val FONT_SIZE_KEY = "font_size"
    private const val DEBUG_KEY = "debug_overlay"
    private const val TAG = "GameTranslatorDebug"
    private const val EMPTY_CLEAR_AFTER_MS = 550L
    private const val MAX_LINES = 18
    private val worker = Executors.newSingleThreadExecutor { r -> Thread(r, "GameTranslator-OCR") }
    private val translationWorker = Executors.newFixedThreadPool(2) { r -> Thread(r, "GameTranslator-Network") }
    private val translationsInFlight = ConcurrentHashMap.newKeySet<String>()
    private val busy = AtomicBoolean(false)
    private val enabledState = mutableStateOf(false)
    private val statusState = mutableStateOf("Выключен")
    private val displayModeState = mutableStateOf(TranslatorDisplayMode.ORIGINAL_AND_TRANSLATION)
    private val glassAlphaState = mutableStateOf(90)
    private val plaqueModeState = mutableStateOf(TranslatorPlaqueMode.GLASS)
    private val fontAutoState = mutableStateOf(true)
    private val fontSizeState = mutableStateOf(22f)
    private val debugState = mutableStateOf(false)
    @Volatile private var appContext: Context? = null
    @Volatile private var lastSource = ""
    @Volatile private var lastTranslationAt = 0L
    @Volatile private var lastTranslationFailureAt = 0L
    @Volatile private var lastVisibleTextAt = 0L
    @Volatile private var candidateText = ""
    @Volatile private var candidateHits = 0
    @Volatile private var emptyFrameHits = 0
    @Volatile private var lastRenderedGeometry = ""
    @Volatile private var lastTranslatedText = ""

    private val latin by lazy { TextRecognition.getClient(TextRecognizerOptions.DEFAULT_OPTIONS) }
    private val japanese by lazy { TextRecognition.getClient(JapaneseTextRecognizerOptions.Builder().build()) }
    private val chinese by lazy { TextRecognition.getClient(ChineseTextRecognizerOptions.Builder().build()) }
    private val korean by lazy { TextRecognition.getClient(KoreanTextRecognizerOptions.Builder().build()) }

    private data class OcrLine(val text: String, val bounds: Rect, val sourceLineCount: Int = 1, val protectedZones: List<Rect> = emptyList())

    val enabled: Boolean get() = enabledState.value
    val status: String get() = statusState.value
    val displayMode: TranslatorDisplayMode get() = displayModeState.value
    val glassAlpha: Int get() = glassAlphaState.value
    val plaqueMode: TranslatorPlaqueMode get() = plaqueModeState.value
    val fontAuto: Boolean get() = fontAutoState.value
    val fontSize: Float get() = fontSizeState.value
    val debugEnabled: Boolean get() = debugState.value

    fun setPlaqueMode(context: Context, mode: TranslatorPlaqueMode) {
        plaqueModeState.value = mode
        lastRenderedGeometry = ""
        context.applicationContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .edit().putString(PLAQUE_MODE_KEY, mode.name).apply()
    }

    fun setGlassAlpha(context: Context, value: Int) {
        val safe = value.coerceIn(40, 100)
        glassAlphaState.value = safe
        lastRenderedGeometry = "" // Repaint the current translation with the new glass opacity.
        context.applicationContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .edit().putInt(GLASS_ALPHA_KEY, safe).apply()
    }

    fun setDebugEnabled(context: Context, enabled: Boolean) {
        debugState.value = enabled
        lastRenderedGeometry = ""
        context.applicationContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .edit().putBoolean(DEBUG_KEY, enabled).apply()
        if (!enabled) {
            lastSource = ""
            WaylandCompositor.nativeSetTranslatorOverlay(byteArrayOf(), 0, 0)
        }
    }

    fun setFontAuto(context: Context, enabled: Boolean) {
        fontAutoState.value = enabled
        lastRenderedGeometry = "" // Repaint without waiting for a new dialogue.
        context.applicationContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .edit().putBoolean(FONT_AUTO_KEY, enabled).apply()
    }

    fun setFontSize(context: Context, value: Float) {
        val safe = value.coerceIn(12f, 36f)
        fontSizeState.value = safe
        lastRenderedGeometry = "" // Repaint without waiting for a new dialogue.
        context.applicationContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .edit().putFloat(FONT_SIZE_KEY, safe).apply()
    }

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
        // This game-translator build is configured to produce Russian output as requested.
        prefs.edit().putString("target", "RU").apply()
        displayModeState.value = runCatching {
            TranslatorDisplayMode.valueOf(prefs.getString(MODE_KEY, null) ?: "")
        }.getOrDefault(TranslatorDisplayMode.ORIGINAL_AND_TRANSLATION)
        glassAlphaState.value = prefs.getInt(GLASS_ALPHA_KEY, 90).coerceIn(40, 100)
        plaqueModeState.value = runCatching { TranslatorPlaqueMode.valueOf(prefs.getString(PLAQUE_MODE_KEY, "GLASS") ?: "GLASS") }.getOrDefault(TranslatorPlaqueMode.GLASS)
        fontAutoState.value = prefs.getBoolean(FONT_AUTO_KEY, true)
        fontSizeState.value = prefs.getFloat(FONT_SIZE_KEY, 22f).coerceIn(12f, 36f)
        debugState.value = prefs.getBoolean(DEBUG_KEY, false)
        lastSource = ""
        candidateText = ""
        candidateHits = 0
        emptyFrameHits = 0
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
        // Callback runs on the compositor thread, so this snapshot reads the live g_map safely.
        val nativeDebugInfo = if (debugState.value) WaylandCompositor.nativeGetTranslatorDebugInfo() else ""
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
                            // Convert OCR rows to paragraph-sized translation units before building the request.
                            val rawOcrLines = lines.map {
                                OcrLineGrouper.Line(it.text, it.bounds.left, it.bounds.top, it.bounds.right, it.bounds.bottom)
                            }
                            if (debugState.value) {
                                OcrLineGrouper.debugDecisions(rawOcrLines).forEach { Log.d(TAG, "paragraph: $it") }
                            }
                            val density = context.resources.displayMetrics.density
                            val iconZones = OcrLineGrouper.buttonGlyphZones(rawOcrLines).map { glyph ->
                                val guard = (8f * density).toInt().coerceAtLeast(6)
                                Rect(
                                    (glyph.left - guard).coerceAtLeast(0),
                                    (glyph.top - guard).coerceAtLeast(0),
                                    glyph.right + guard,
                                    glyph.bottom + guard
                                )
                            }
                            val groupedLines = OcrLineGrouper.group(rawOcrLines).take(MAX_LINES)
                            val clean = groupedLines.map {
                                OcrLine(it.text, Rect(it.left, it.top, it.right, it.bottom), it.lineCount, iconZones)
                            }
                            if (debugState.value && lastTranslatedText.isBlank()) renderDebugOverlay(width, height, clean, nativeDebugInfo)
                            val text = clean.joinToString("\n") { it.text }.take(1400)
                            val normalized = text.lowercase().replace(Regex("\\s+"), " ").trim()
                            val now = System.currentTimeMillis()
                            if (normalized.isBlank()) {
                                // Two consecutive empty OCR results suppress one-frame OCR jitter.
                                emptyFrameHits++
                                if (emptyFrameHits >= 2 || now - lastVisibleTextAt > EMPTY_CLEAR_AFTER_MS) {
                                    if (lastSource.isNotEmpty() || lastTranslatedText.isNotEmpty()) {
                                        lastSource = ""
                                        lastTranslatedText = ""
                                        lastRenderedGeometry = ""
                                        candidateText = ""
                                        candidateHits = 0
                                        WaylandCompositor.nativeSetTranslatorOverlay(byteArrayOf(), 0, 0)
                                    }
                                    emptyFrameHits = 0
                                    statusState.value = "Turnip: текста в кадре нет"
                                } else {
                                    statusState.value = "OCR: проверяю исчезновение текста…"
                                }
                            } else {
                                emptyFrameHits = 0
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
                                            lastTranslatedText, displayModeState.value, glassAlphaState.value, fontAutoState.value, fontSizeState.value, plaqueModeState.value)
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
                        if (System.currentTimeMillis() - lastVisibleTextAt > EMPTY_CLEAR_AFTER_MS) {
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
                drawModeOverlay(frameBitmap, width, height, lines, translated, displayModeState.value, glassAlphaState.value, fontAutoState.value, fontSizeState.value, plaqueModeState.value)
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

    private fun resolveGroupFontSize(
        translations: List<String>,
        lines: List<OcrLine>,
        sceneWidth: Int,
        sceneHeight: Int,
        baseSize: Float,
        protectedZones: List<Rect>
    ): Float {
        if (translations.isEmpty() || lines.isEmpty()) return baseSize
        val density = appContext?.resources?.displayMetrics?.density ?: 1f
        fun availableWidth(line: OcrLine): Int {
            val box = line.bounds
            val center = box.centerX()
            val isCentered = kotlin.math.abs(center - sceneWidth / 2f) <= sceneWidth * 0.10f
            val isRight = !isCentered && box.right >= sceneWidth * 0.68f
            var leftLimit = 0
            var rightLimit = sceneWidth
            protectedZones.forEach { zone ->
                if (zone.bottom > box.top - box.height() * 0.25f && zone.top < box.bottom + box.height() * 0.25f) {
                    if (zone.right <= box.left) leftLimit = max(leftLimit, zone.right)
                    if (zone.left >= box.right) rightLimit = min(rightLimit, zone.left)
                }
            }
            return when {
                isCentered -> (2 * min(center - leftLimit, rightLimit - center)).toInt()
                isRight -> box.right - leftLimit
                else -> rightLimit - box.left
            }.coerceAtLeast(box.width())
        }
        val minLineHeight = lines.minOf { (it.bounds.height() / it.sourceLineCount.coerceAtLeast(1)).coerceAtLeast(1) }
        val verticalLimit = minLineHeight + (5f * density).toInt()
        var low = baseSize * 0.70f
        var high = baseSize
        repeat(10) {
            val candidate = (low + high) * 0.5f
            val fits = translations.indices.all { index ->
                val p = TextPaint(Paint.ANTI_ALIAS_FLAG).apply {
                    textSize = candidate
                    typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
                }
                val fm = p.fontMetrics
                val heightFits = (fm.descent - fm.ascent) * lines[index].sourceLineCount <=
                    lines[index].bounds.height() + (5f * density)
                val widthFits = lines[index].sourceLineCount > 1 ||
                    p.measureText(translations[index]) <= availableWidth(lines[index]) - 2f * 5f * density
                heightFits && widthFits && (fm.descent - fm.ascent) <= verticalLimit
            }
            if (fits) low = candidate else high = candidate
        }
        return low.coerceIn(baseSize * 0.70f, baseSize)
    }

    private fun drawModeOverlay(
        sourceFrame: Bitmap,
        sceneWidth: Int,
        sceneHeight: Int,
        lines: List<OcrLine>,
        translated: String,
        mode: TranslatorDisplayMode,
        glassAlpha: Int,
        fontAuto: Boolean,
        manualFontSize: Float,
        plaqueMode: TranslatorPlaqueMode
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
        // A single baseline font size is shared by the entire OCR group (especially menu rows).
        // It is derived from the shortest source row, never independently fitted per translation.
        val groupBaseSize = if (fontAuto) {
            (lines.minOfOrNull { it.bounds.height().toFloat() / it.sourceLineCount.coerceAtLeast(1) }?.times(0.90f) ?: 22f).coerceIn(9.5f, 42f)
        } else (manualFontSize * sceneWidth / 1080f).coerceIn(10f, 72f)
        val protectedZones = lines.flatMap { it.protectedZones }.distinct()
        val groupFontSize = resolveGroupFontSize(translations, lines, sceneWidth, sceneHeight, groupBaseSize, protectedZones)
        val textPaint = TextPaint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.WHITE
            textSize = groupFontSize
            typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
            setShadowLayer(2f, 0.5f, 1f, 0x99000000.toInt())
        }
        val stroke = TextPaint(textPaint).apply {
            color = Color.BLACK
            style = Paint.Style.STROKE
            strokeWidth = max(1.2f, textPaint.textSize / 12f)
            clearShadowLayer()
        }

        val occupiedPlaques = ArrayList<Rect>()
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
                        drawFitted(canvas, translatedLine, box.left, y, width, safeHeight, textPaint, stroke, box.height())
                    }
                    TranslatorDisplayMode.REPLACE_TEXT -> {
                        // Frosted-glass patch: downsampled live scene provides a soft blur, then
                        // a translucent cool tint and fine highlight create a glass-like surface.
                        // Size the glass strictly from the translated glyph bounds. The old
                        // 2.6x source-width heuristic created oversized empty panels.
                        val occupiedRects = occupiedPlaques
                        val drawn = drawCompactGlassText(
                            canvas, sourceFrame, translatedLine, box,
                            glassAlpha, groupFontSize, textPaint, stroke, plaqueMode,
                            line.sourceLineCount, protectedZones, occupiedRects
                        )
                        if (drawn != null) occupiedPlaques += drawn
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
                                rightRect.width(), rightRect.height(), textPaint, stroke, box.height())
                        } else {
                            val nextTop = lines.filter { it.bounds.top > box.top }
                                .minOfOrNull { it.bounds.top } ?: sceneHeight
                            val belowY = box.bottom + margin
                            val available = (nextTop - belowY).coerceAtLeast(0)
                            val belowRect = Rect(box.left, belowY, sceneWidth - 8, belowY + available)
                            val overlapsBelow = lines.any { other ->
                                other !== line && Rect.intersects(belowRect, other.bounds)
                            }
                            if (available >= 24 && !overlapsBelow) {
                                drawFitted(canvas, translatedLine, box.left, belowY,
                                    (sceneWidth - box.left - 8).coerceAtLeast(box.width()),
                                    available, textPaint, stroke, box.height())
                            }
                        }
                    }
                }
            }
        }
        if (debugState.value) drawDebugBounds(canvas, sceneWidth, sceneHeight, lines)

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

    /**
     * Draws a tight glass chip around the actual translated text, not around an
     * arbitrary fraction of the source line width. Long translations wrap only
     * when they need to and the panel grows by the measured glyph width/height.
     */
    /**
     * REPLACE_TEXT uses the OCR rectangle as a hard placement contract:
     * the glass patch and translated glyph area stay inside the exact source
     * label bounds. Never center a newly-sized panel around the anchor, since
     * that makes the replacement drift when translation length changes.
     */
    private fun drawCompactGlassText(
        canvas: Canvas,
        sourceFrame: Bitmap,
        text: String,
        anchor: Rect,
        opacity: Int,
        groupFontSize: Float,
        paint: TextPaint,
        stroke: TextPaint,
        plaqueMode: TranslatorPlaqueMode,
        sourceLineCount: Int,
        protectedZones: List<Rect>,
        occupied: List<Rect>
    ): Rect? {
        if (text.isBlank() || anchor.width() <= 2 || anchor.height() <= 2) return null

        val density = appContext?.resources?.displayMetrics?.density ?: 1f
        val padX = (5f * density).toInt().coerceAtLeast(4)
        val padY = min((2.5f * density).toInt().coerceAtLeast(1), (anchor.height() * 0.12f).toInt().coerceAtLeast(1))
        val innerMaskPad = min((1.5f * density).toInt().coerceAtLeast(1), padY)
        val centered = kotlin.math.abs(anchor.centerX() - canvas.width / 2f) <= canvas.width * 0.10f
        val rightAligned = !centered && anchor.right >= canvas.width * 0.68f

        var leftLimit = 0
        var rightLimit = canvas.width
        protectedZones.forEach { zone ->
            if (zone.bottom > anchor.top - anchor.height() * 0.25f &&
                zone.top < anchor.bottom + anchor.height() * 0.25f) {
                if (zone.right <= anchor.left) leftLimit = max(leftLimit, zone.right)
                if (zone.left >= anchor.right) rightLimit = min(rightLimit, zone.left)
            }
        }
        val available = when {
            centered -> (2f * min(anchor.centerX() - leftLimit, rightLimit - anchor.centerX())).toInt()
            rightAligned -> anchor.right - leftLimit
            else -> rightLimit - anchor.left
        }.coerceAtLeast(anchor.width())

        val originalSize = paint.textSize
        var size = groupFontSize
        var measuredWidth: Float
        var layout: StaticLayout? = null
        var strokeLayout: StaticLayout? = null
        var contentWidth: Int
        var plaque: Rect
        val minSize = groupFontSize * 0.70f
        val allowWrap = sourceLineCount > 1
        val alignment = when {
            centered -> Layout.Alignment.ALIGN_CENTER
            rightAligned -> Layout.Alignment.ALIGN_OPPOSITE
            else -> Layout.Alignment.ALIGN_NORMAL
        }

        fun buildLayout(candidate: Float, width: Int): Pair<StaticLayout, StaticLayout> {
            paint.textSize = candidate
            stroke.textSize = candidate
            val safeText = if (!allowWrap && paint.measureText(text) > width) {
                ellipsizeAtWord(text, paint, width)
            } else text
            val maxLines = if (allowWrap) sourceLineCount.coerceIn(2, 4) else 1
            val main = StaticLayout.Builder.obtain(safeText, 0, safeText.length, paint, width)
                .setAlignment(alignment).setIncludePad(false).setMaxLines(maxLines)
                .setEllipsize(TextUtils.TruncateAt.END).setBreakStrategy(Layout.BREAK_STRATEGY_HIGH_QUALITY).build()
            val outline = StaticLayout.Builder.obtain(safeText, 0, safeText.length, stroke, width)
                .setAlignment(alignment).setIncludePad(false).setMaxLines(maxLines)
                .setEllipsize(TextUtils.TruncateAt.END).setBreakStrategy(Layout.BREAK_STRATEGY_HIGH_QUALITY).build()
            return main to outline
        }

        var chosen = false
        var bestRect: Rect? = null
        while (size >= minSize - 0.01f) {
            paint.textSize = size
            stroke.textSize = size
            measuredWidth = if (allowWrap) min(available.toFloat(), paint.measureText(text)) else paint.measureText(text)
            val content = max(anchor.width(), measuredWidth.toInt())
            val width = min(available - padX * 2, content).coerceAtLeast(anchor.width() - 2)
            contentWidth = width
            val outerWidth = contentWidth + padX * 2
            val left = when {
                centered -> anchor.centerX() - outerWidth / 2
                rightAligned -> anchor.right - contentWidth - padX
                else -> anchor.left - padX
            }.coerceAtLeast(0)
            val right = (left + outerWidth).coerceAtMost(canvas.width)
            plaque = Rect(left, (anchor.top - padY).coerceAtLeast(0), right,
                (anchor.bottom + padY).coerceAtMost(canvas.height))
            if (plaque.width() < outerWidth) {
                if (rightAligned) plaque.left = (plaque.right - outerWidth).coerceAtLeast(0)
                else if (centered) {
                    plaque.left = (anchor.centerX() - outerWidth / 2).coerceAtLeast(0)
                    plaque.right = (plaque.left + outerWidth).coerceAtMost(canvas.width)
                }
            }
            val iconCollision = protectedZones.any { Rect.intersects(plaque, it) }
            val plaqueCollision = occupied.any { Rect.intersects(plaque, it) }
            val (candidateLayout, candidateStroke) = buildLayout(size, contentWidth)
            val maxTextHeight = anchor.height() + padY * 2
            val heightFits = candidateLayout.height <= maxTextHeight
            if (!iconCollision && !plaqueCollision && heightFits &&
                (allowWrap || paint.measureText(text) <= contentWidth + 1f)) {
                layout = candidateLayout
                strokeLayout = candidateStroke
                bestRect = plaque
                chosen = true
                break
            }
            size -= max(0.5f, groupFontSize * 0.035f)
        }

        if (!chosen || bestRect == null) {
            // Preserve a one-line label rather than wrapping a word or painting over an icon.
            size = minSize
            paint.textSize = size
            stroke.textSize = size
            contentWidth = (available - padX * 2).coerceAtLeast(anchor.width() - 2)
            val fallbackWidth = max(anchor.width(), min(contentWidth, paint.measureText(text).toInt()))
            val outerWidth = fallbackWidth + padX * 2
            val left = when {
                centered -> (anchor.centerX() - outerWidth / 2).coerceAtLeast(0)
                rightAligned -> (anchor.right - fallbackWidth - padX).coerceAtLeast(0)
                else -> (anchor.left - padX).coerceAtLeast(0)
            }
            bestRect = Rect(left, (anchor.top - padY).coerceAtLeast(0),
                min(canvas.width, left + outerWidth), (anchor.bottom + padY).coerceAtMost(canvas.height))
            if (protectedZones.any { Rect.intersects(bestRect!!, it) } || occupied.any { Rect.intersects(bestRect!!, it) }) {
                paint.textSize = originalSize
                stroke.textSize = originalSize
                return null
            }
            contentWidth = (bestRect!!.width() - padX * 2).coerceAtLeast(1)
            val safeText = if (allowWrap) text else ellipsizeAtWord(text, paint, contentWidth)
            layout = StaticLayout.Builder.obtain(safeText, 0, safeText.length, paint, contentWidth)
                .setAlignment(alignment).setIncludePad(false).setMaxLines(if (allowWrap) sourceLineCount.coerceIn(2, 4) else 1)
                .setEllipsize(TextUtils.TruncateAt.END).build()
            strokeLayout = StaticLayout.Builder.obtain(safeText, 0, safeText.length, stroke, contentWidth)
                .setAlignment(alignment).setIncludePad(false).setMaxLines(if (allowWrap) sourceLineCount.coerceIn(2, 4) else 1)
                .setEllipsize(TextUtils.TruncateAt.END).build()
        }

        val rect = bestRect!!
        val finalLayout = layout ?: return null
        val finalStrokeLayout = strokeLayout ?: return null
        val sampled = sampleBorderColor(sourceFrame, anchor, (3f * density).toInt().coerceAtLeast(2))
        val variance = borderLuminanceVariance(sourceFrame, anchor, (3f * density).toInt().coerceAtLeast(2))
        val resolvedMode = when (plaqueMode) {
            TranslatorPlaqueMode.DENSE -> TranslatorPlaqueMode.DENSE
            TranslatorPlaqueMode.GLASS -> TranslatorPlaqueMode.GLASS
            TranslatorPlaqueMode.AUTO ->
                if (variance <= 0.035f) TranslatorPlaqueMode.GLASS else TranslatorPlaqueMode.DENSE
        }
        val base = if (resolvedMode == TranslatorPlaqueMode.DENSE)
            blendColors(sampled, Color.rgb(30, 34, 40), 0.82f)
        else blendColors(sampled, Color.rgb(30, 34, 40), 0.42f)
        val corner = (rect.height() * 0.30f).coerceAtLeast(3f)
        val rounded = android.graphics.RectF(rect)

        if (resolvedMode == TranslatorPlaqueMode.GLASS) {
            // Frosted live backdrop. The original glyph area is then repainted opaquely.
            drawBlurredCrop(canvas, sourceFrame, rect)
            val mask = android.graphics.RectF(
                (anchor.left - innerMaskPad).coerceAtLeast(rect.left).toFloat(),
                (anchor.top - innerMaskPad).coerceAtLeast(rect.top).toFloat(),
                (anchor.right + innerMaskPad).coerceAtMost(rect.right).toFloat(),
                (anchor.bottom + innerMaskPad).coerceAtMost(rect.bottom).toFloat()
            )
            canvas.drawRoundRect(mask, (mask.height() * 0.30f).coerceAtLeast(2f),
                (mask.height() * 0.30f).coerceAtLeast(2f),
                Paint(Paint.ANTI_ALIAS_FLAG).apply { color = base; alpha = 255; style = Paint.Style.FILL })
        } else {
            canvas.drawRoundRect(rounded, corner, corner,
                Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.rgb(30, 34, 40); alpha = 255; style = Paint.Style.FILL })
        }

        val tintAlpha = opacity.coerceIn(40, 100) * 255 / 100
        val tint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            shader = LinearGradient(
                0f, rect.top.toFloat(), 0f, rect.bottom.toFloat(),
                Color.argb(tintAlpha, 24, 29, 36),
                Color.argb((tintAlpha * 0.72f).toInt(), 52, 62, 76),
                Shader.TileMode.CLAMP
            )
        }
        canvas.drawRoundRect(rounded, corner, corner, tint)
        val finalBase = blendColors(base, Color.rgb(24, 29, 36), tintAlpha / 255f)
        val whiteContrast = contrastRatio(finalBase, Color.WHITE)
        val blackContrast = contrastRatio(finalBase, Color.BLACK)
        val textColor = if (whiteContrast >= blackContrast) Color.WHITE else Color.BLACK
        val borderColor = if (textColor == Color.WHITE) Color.argb(82, 170, 190, 205) else Color.argb(82, 50, 60, 70)
        canvas.drawRoundRect(rounded, corner, corner,
            Paint(Paint.ANTI_ALIAS_FLAG).apply {
                color = borderColor
                style = Paint.Style.STROKE
                strokeWidth = density
            })
        paint.color = textColor
        paint.letterSpacing = 0.012f
        paint.setShadowLayer(1.2f * density, 0f, 0.6f * density,
            if (textColor == Color.WHITE) Color.BLACK else Color.WHITE)
        stroke.color = if (textColor == Color.WHITE) Color.BLACK else Color.WHITE
        stroke.strokeWidth = max(1f, 1.1f * density)
        stroke.setShadowLayer(1f * density, 0f, 0f, if (textColor == Color.WHITE) Color.BLACK else Color.WHITE)

        val textTop = rect.top + ((rect.height() - finalLayout.height) / 2f).coerceAtLeast(0f)
        canvas.save()
        canvas.clipRect(rect)
        canvas.translate((rect.left + padX).toFloat(), textTop)
        finalStrokeLayout.draw(canvas)
        finalLayout.draw(canvas)
        canvas.restore()
        paint.clearShadowLayer()
        stroke.clearShadowLayer()
        paint.textSize = originalSize
        stroke.textSize = originalSize
        Log.d(TAG, "plaque sourceH=${anchor.height()} plaqueH=${rect.height()} mode=$resolvedMode variance=$variance font=$size contrast=${max(whiteContrast, blackContrast)}")
        return rect
    }

    private fun ellipsizeAtWord(text: String, paint: TextPaint, width: Int): String {
        val ellipsis = "…"
        if (paint.measureText(text) <= width) return text
        val words = text.trim().split(Regex("\\s+")).toMutableList()
        while (words.isNotEmpty()) {
            val candidate = words.joinToString(" ") + ellipsis
            if (paint.measureText(candidate) <= width) return candidate
            words.removeAt(words.lastIndex)
        }
        return ellipsis
    }

    private fun sampleBorderColor(frame: Bitmap, box: Rect, radius: Int): Int {
        val values = ArrayList<Int>(128)
        val r = radius.coerceIn(2, 6)
        val left = (box.left - r).coerceAtLeast(0)
        val top = (box.top - r).coerceAtLeast(0)
        val right = (box.right + r).coerceAtMost(frame.width - 1)
        val bottom = (box.bottom + r).coerceAtMost(frame.height - 1)
        for (y in top..bottom step 2) for (x in left..right step 2) {
            if (x >= box.left && x <= box.right && y >= box.top && y <= box.bottom) continue
            val color = frame.getPixel(x, y)
            if (Color.alpha(color) > 180) values += color
        }
        if (values.isEmpty()) return Color.rgb(30, 34, 40)
        fun median(channel: (Int) -> Int): Int {
            val sorted = values.map(channel).sorted()
            return sorted[sorted.size / 2]
        }
        return Color.rgb(median(Color::red), median(Color::green), median(Color::blue))
    }

    private fun borderLuminanceVariance(frame: Bitmap, box: Rect, radius: Int): Float {
        val samples = ArrayList<Float>(128)
        val r = radius.coerceIn(2, 6)
        val left = (box.left - r).coerceAtLeast(0)
        val top = (box.top - r).coerceAtLeast(0)
        val right = (box.right + r).coerceAtMost(frame.width - 1)
        val bottom = (box.bottom + r).coerceAtMost(frame.height - 1)
        for (y in top..bottom step 2) for (x in left..right step 2) {
            if (x >= box.left && x <= box.right && y >= box.top && y <= box.bottom) continue
            val color = frame.getPixel(x, y)
            val lum = (0.2126f * Color.red(color) + 0.7152f * Color.green(color) + 0.0722f * Color.blue(color)) / 255f
            samples += lum
        }
        if (samples.size < 2) return 1f
        val mean = samples.average().toFloat()
        return samples.sumOf { ((it - mean) * (it - mean)).toDouble() }.toFloat() / samples.size
    }

    private fun blendColors(a: Int, b: Int, amount: Float): Int {
        val t = amount.coerceIn(0f, 1f)
        return Color.rgb(
            (Color.red(a) * (1f - t) + Color.red(b) * t).toInt(),
            (Color.green(a) * (1f - t) + Color.green(b) * t).toInt(),
            (Color.blue(a) * (1f - t) + Color.blue(b) * t).toInt()
        )
    }

    private fun linearChannel(value: Int): Float {
        val c = value / 255f
        return if (c <= 0.04045f) c / 12.92f else Math.pow(((c + 0.055f) / 1.055f).toDouble(), 2.4).toFloat()
    }

    private fun contrastRatio(background: Int, foreground: Int): Float {
        fun luminance(c: Int) = 0.2126f * linearChannel(Color.red(c)) +
            0.7152f * linearChannel(Color.green(c)) + 0.0722f * linearChannel(Color.blue(c))
        val l1 = luminance(background)
        val l2 = luminance(foreground)
        return (max(l1, l2) + 0.05f) / (min(l1, l2) + 0.05f)
    }

    private fun drawBlurredCrop(canvas: Canvas, frame: Bitmap, rect: Rect) {
        val safe = Rect(rect)
        if (!safe.intersect(0, 0, frame.width, frame.height) || safe.width() <= 1 || safe.height() <= 1) return
        val crop = Bitmap.createBitmap(frame, safe.left, safe.top, safe.width(), safe.height())
        val small = Bitmap.createScaledBitmap(crop, max(1, crop.width / 6), max(1, crop.height / 6), true)
        val blurred = Bitmap.createScaledBitmap(small, crop.width, crop.height, true)
        canvas.drawBitmap(blurred, null, android.graphics.RectF(safe), Paint(Paint.ANTI_ALIAS_FLAG or Paint.FILTER_BITMAP_FLAG))
        crop.recycle()
        small.recycle()
        blurred.recycle()
    }

    private fun drawFrostedGlass(
        canvas: Canvas,
        blurredScene: Bitmap,
        rect: Rect,
        sceneWidth: Int,
        sceneHeight: Int,
        opacity: Int
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
        // The blur layer is an opaque mask: the original glyphs must never show
        // through, even when the user sets glass tint opacity to zero.
        canvas.drawBitmap(blurredScene, src, android.graphics.RectF(rect),
            Paint(Paint.ANTI_ALIAS_FLAG or Paint.FILTER_BITMAP_FLAG).apply { alpha = 255 })
        // Opacity controls only the glass tint/highlight, not the masking layer.
        canvas.drawRoundRect(android.graphics.RectF(rect), radius, radius,
            Paint(Paint.ANTI_ALIAS_FLAG).apply {
                color = ((opacity * 2.0f).toInt().coerceIn(0, 200) shl 24) or 0x005b7188
                style = Paint.Style.FILL
            })
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

    /**
     * Diagnostics are expressed in compositor-scene pixels. The JNI callback currently exports
     * frame dimensions plus the native output viewport and the actual scene-to-output matrix.
     * The overlay is a full-scene texture positioned at the scene origin (0,0).
     */
    private fun renderDebugOverlay(width: Int, height: Int, lines: List<OcrLine>, nativeInfo: String) {
        Log.d(TAG, "frame=${width}x${height} native=$nativeInfo")
        val matrix = Regex("scale=\\(([0-9.]+),([0-9.]+)\\) offset=\\((-?[0-9.]+),(-?[0-9.]+)\\)")
            .find(nativeInfo)?.groupValues
        val sx = matrix?.get(1)?.toFloatOrNull() ?: 1f
        val sy = matrix?.get(2)?.toFloatOrNull() ?: 1f
        val ox = matrix?.get(3)?.toFloatOrNull() ?: 0f
        val oy = matrix?.get(4)?.toFloatOrNull() ?: 0f
        lines.forEachIndexed { index, line ->
            val b = line.bounds
            val expanded = expandedOverlayBounds(b, width, height)
            Log.d(TAG, "ocr[$index] frameBBox=(${b.left},${b.top},${b.right},${b.bottom}) " +
                "expandedOverlayBBox=(${expanded.left},${expanded.top},${expanded.right},${expanded.bottom}) " +
                "outputBBox=(${ox + b.left * sx},${oy + b.top * sy},${ox + b.right * sx},${oy + b.bottom * sy}) " +
                "outputExpandedBBox=(${ox + expanded.left * sx},${oy + expanded.top * sy},${ox + expanded.right * sx},${oy + expanded.bottom * sy}) " +
                "text=${line.text.take(120)}")
        }
        if (lines.isEmpty()) {
            WaylandCompositor.nativeSetTranslatorOverlay(byteArrayOf(), 0, 0)
            return
        }
        val bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
        drawDebugBounds(Canvas(bitmap), width, height, lines)
        val pixels = IntArray(width * height)
        bitmap.getPixels(pixels, 0, width, 0, 0, width, height)
        val rgba = ByteArray(pixels.size * 4)
        var offset = 0
        for (pixel in pixels) {
            rgba[offset++] = ((pixel shr 16) and 0xff).toByte()
            rgba[offset++] = ((pixel shr 8) and 0xff).toByte()
            rgba[offset++] = (pixel and 0xff).toByte()
            rgba[offset++] = ((pixel ushr 24) and 0xff).toByte()
        }
        bitmap.recycle()
        WaylandCompositor.nativeSetTranslatorOverlay(rgba, width, height)
    }

    private fun expandedOverlayBounds(source: Rect, width: Int, height: Int): Rect {
        val density = appContext?.resources?.displayMetrics?.density ?: 1f
        val padX = (5f * density).toInt().coerceAtLeast(4)
        val padY = min((2.5f * density).toInt().coerceAtLeast(1), (source.height() * 0.12f).toInt().coerceAtLeast(1))
        return Rect((source.left - padX).coerceAtLeast(0), (source.top - padY).coerceAtLeast(0),
            (source.right + padX).coerceAtMost(width), (source.bottom + padY).coerceAtMost(height))
    }

    private fun drawDebugBounds(canvas: Canvas, width: Int, height: Int, lines: List<OcrLine>) {
        val edge = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.GREEN
            style = Paint.Style.STROKE
            strokeWidth = (width / 540f).coerceIn(2f, 5f)
        }
        canvas.drawRect(1f, 1f, (width - 1).toFloat(), (height - 1).toFloat(), edge)
        val cross = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.YELLOW; strokeWidth = 2f }
        fun mark(x: Float, y: Float) {
            canvas.drawLine(x - 10f, y, x + 10f, y, cross)
            canvas.drawLine(x, y - 10f, x, y + 10f, cross)
        }
        mark(0f, 0f); mark(width / 2f, height / 2f); mark(width.toFloat(), height.toFloat())
        val original = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.GREEN; style = Paint.Style.STROKE; strokeWidth = (width / 540f).coerceIn(2f, 5f)
        }
        val plaque = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.BLUE; style = Paint.Style.STROKE; strokeWidth = (width / 540f).coerceIn(2f, 5f)
        }
        val label = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.WHITE
            textSize = (width / 48f).coerceIn(14f, 28f)
            style = Paint.Style.FILL
        }
        val background = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = 0xcc003344.toInt()
            style = Paint.Style.FILL
        }
        lines.forEachIndexed { index, line ->
            val b = Rect(line.bounds)
            b.intersect(0, 0, width, height)
            if (b.width() <= 0 || b.height() <= 0) return@forEachIndexed
            canvas.drawRect(b, original)
            canvas.drawRect(expandedOverlayBounds(b, width, height), plaque)
            line.protectedZones.forEach { zone ->
                val iconZone = Paint(Paint.ANTI_ALIAS_FLAG).apply {
                    color = Color.YELLOW; style = Paint.Style.STROKE; strokeWidth = 2f
                }
                canvas.drawRect(zone, iconZone)
            }
            val caption = "OCR $index h=${b.height()} p=${expandedOverlayBounds(b, width, height).height()}"
            val captionWidth = label.measureText(caption) + 10f
            val top = (b.top - label.textSize - 8f).coerceAtLeast(0f)
            canvas.drawRect(b.left.toFloat(), top, b.left + captionWidth, top + label.textSize + 6f, background)
            canvas.drawText(caption, b.left + 5f, top + label.textSize, label)
        }
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
        paint: TextPaint, stroke: TextPaint, anchorHeight: Int
    ) {
        if (maxWidth <= 8 || maxHeight <= 8 || text.isBlank()) return
        var size = if (fontAutoState.value) (anchorHeight * 0.88f).coerceIn(12f, 42f) else paint.textSize
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
        paint.textSize = if (fontAutoState.value) (canvas.width / 58f).coerceIn(15f, 30f)
            else (fontSizeState.value * canvas.width / 1080f).coerceIn(10f, 72f)
        stroke.textSize = paint.textSize
    }
}

@Composable
internal fun GameTranslatorPanel(paneScale: Float) {
    val context = LocalContext.current
    val enabled = GameTranslator.enabled
    val status = GameTranslator.status
    val selectedMode = GameTranslator.displayMode
    val glassAlpha = GameTranslator.glassAlpha
    val fontAuto = GameTranslator.fontAuto
    val fontSize = GameTranslator.fontSize
    val debugEnabled = GameTranslator.debugEnabled
    var menuExpanded by remember { mutableStateOf(false) }
    Column(verticalArrangement = Arrangement.spacedBy((6f * paneScale).dp)) {
        Text("Игровой переводчик", color = MaterialTheme.colorScheme.onSurface, fontSize = (13f * paneScale).sp)
        Button(
            onClick = { if (enabled) GameTranslator.stop() else GameTranslator.start(context) },
            modifier = Modifier.fillMaxWidth()
        ) { Text(if (enabled) "ОСТАНОВИТЬ ПЕРЕВОД" else "ВКЛЮЧИТЬ ПЕРЕВОД В ИГРЕ") }
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = androidx.compose.ui.Alignment.CenterVertically) {
            Button(onClick = { menuExpanded = true }, modifier = Modifier.weight(1f)) {
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
        if (selectedMode == TranslatorDisplayMode.REPLACE_TEXT) {
            Column {
                Text("Прозрачность стекла: $glassAlpha%", color = MaterialTheme.colorScheme.onSurfaceVariant,
                    fontSize = (10f * paneScale).sp)
                Slider(value = glassAlpha.toFloat(), onValueChange = { GameTranslator.setGlassAlpha(context, it.toInt()) },
                    valueRange = 40f..100f)
            }
        }
        Row(verticalAlignment = androidx.compose.ui.Alignment.CenterVertically) {
            Text("Debug OCR: рамки и координаты", modifier = Modifier.weight(1f),
                color = MaterialTheme.colorScheme.onSurfaceVariant, fontSize = (10f * paneScale).sp)
            Switch(checked = debugEnabled, onCheckedChange = { GameTranslator.setDebugEnabled(context, it) })
        }
        Row(verticalAlignment = androidx.compose.ui.Alignment.CenterVertically) {
            Text("Размер шрифта: ${if (fontAuto) "Авто" else "Ручной"}", modifier = Modifier.weight(1f),
                color = MaterialTheme.colorScheme.onSurfaceVariant, fontSize = (10f * paneScale).sp)
            Switch(checked = !fontAuto, onCheckedChange = { GameTranslator.setFontAuto(context, !it) })
        }
        if (!fontAuto) {
            Text("Размер: ${fontSize.toInt()}", color = MaterialTheme.colorScheme.onSurfaceVariant,
                fontSize = (10f * paneScale).sp)
            Slider(value = fontSize, onValueChange = { GameTranslator.setFontSize(context, it) },
                valueRange = 12f..36f)
        }
        Text(selectedMode.description, color = MaterialTheme.colorScheme.onSurfaceVariant,
            fontSize = (10f * paneScale).sp)
        Text("Turnip/Vulkan → OCR → перевод с привязкой к исходным координатам → Vulkan overlay",
            color = MaterialTheme.colorScheme.onSurfaceVariant, fontSize = (10f * paneScale).sp)
        Text(status, color = MaterialTheme.colorScheme.onSurfaceVariant, fontSize = (11f * paneScale).sp)
    }
}
