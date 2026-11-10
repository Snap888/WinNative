package com.winlator.cmod.feature.translator

import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.graphics.PixelFormat
import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.media.Image
import android.media.ImageReader
import android.media.projection.MediaProjection
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.IBinder
import android.provider.Settings
import android.view.Gravity
import android.view.View
import android.view.WindowManager
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView
import androidx.core.app.NotificationCompat
import com.google.mlkit.vision.common.InputImage
import com.google.mlkit.vision.text.TextRecognition
import com.google.mlkit.vision.text.latin.TextRecognizerOptions
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder
import java.util.concurrent.Executors

class TranslatorOverlayService : Service() {
    private var projection: MediaProjection? = null
    private var display: VirtualDisplay? = null
    private var reader: ImageReader? = null
    private var windowManager: WindowManager? = null
    private var root: View? = null
    private var panel: View? = null
    private var output: TextView? = null
    private var capturing = true
    private var lastFrame = 0L
    private var lastText = ""
    private var lastTranslation = ""
    private val executor = Executors.newSingleThreadExecutor()
    private val recognizer by lazy { TextRecognition.getClient(TextRecognizerOptions.DEFAULT_OPTIONS) }
    private lateinit var thread: HandlerThread
    private lateinit var handler: Handler

    override fun onCreate() {
        super.onCreate()
        thread = HandlerThread("WinTranslator-OCR").apply { start() }
        handler = Handler(thread.looper)
        val channel = NotificationChannel(CHANNEL, "WinTranslator", NotificationManager.IMPORTANCE_LOW)
        getSystemService(NotificationManager::class.java).createNotificationChannel(channel)
        val notification = NotificationCompat.Builder(this, CHANNEL)
            .setSmallIcon(android.R.drawable.ic_menu_info_details).setContentTitle("WinTranslator AI")
            .setContentText("Игровой переводчик активен").setOngoing(true).build()
        if (Build.VERSION.SDK_INT >= 29) startForeground(NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION)
        else startForeground(NOTIFICATION_ID, notification)
        if (Settings.canDrawOverlays(this)) createOverlay()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) { stopSelf(); return START_NOT_STICKY }
        if (intent?.action == ACTION_PAUSE) { capturing = !capturing; output?.text = if (capturing) "Ожидание текста…" else "Перевод на паузе"; return START_STICKY }
        val code = intent?.getIntExtra(EXTRA_CODE, 0) ?: 0
        val data: Intent? = if (Build.VERSION.SDK_INT >= 33) intent?.getParcelableExtra(EXTRA_DATA, Intent::class.java) else @Suppress("DEPRECATION") intent?.getParcelableExtra(EXTRA_DATA)
        if (data != null && projection == null) startCapture(code, data)
        return START_STICKY
    }

    private fun startCapture(code: Int, data: Intent) {
        try {
            val manager = getSystemService(Context.MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
            projection = manager.getMediaProjection(code, data)
            val metrics = resources.displayMetrics
            val width = metrics.widthPixels.coerceAtLeast(1)
            val height = metrics.heightPixels.coerceAtLeast(1)
            reader = ImageReader.newInstance(width, height, 2, PixelFormat.RGBA_8888)
            reader?.setOnImageAvailableListener({ source ->
                val image = source.acquireLatestImage() ?: return@setOnImageAvailableListener
                val now = System.currentTimeMillis()
                if (!capturing || now - lastFrame < 1500L) { image.close(); return@setOnImageAvailableListener }
                lastFrame = now
                recognize(image)
            }, handler)
            projection?.registerCallback(object : MediaProjection.Callback() {
                override fun onStop() { stopSelf() }
            }, handler)
            display = projection?.createVirtualDisplay("WinTranslator", width, height, metrics.densityDpi,
                DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR, reader?.surface, null, handler)
        } catch (t: Throwable) { output?.text = "Захват экрана: " + (t.message ?: "ошибка") }
    }

    private fun recognize(image: Image) {
        val input = try { InputImage.fromMediaImage(image, 0) } catch (t: Throwable) { image.close(); return }
        recognizer.process(input).addOnSuccessListener { result ->
            val ownLines = lastTranslation.lines().map { it.trim() }.filter { it.isNotEmpty() }
            val text = result.text.lines().map { it.trim() }
                .filter { it.length >= 2 && !it.equals("ОРИГИНАЛ", true) && !it.equals("ПЕРЕВОД", true) }
                .filterNot { line -> ownLines.any { it.equals(line, ignoreCase = true) } }
                .distinct().joinToString("\n").take(1800)
            val normalized = text.lowercase().replace(Regex("\\s+"), " ").trim()
            val previous = lastText.lowercase().replace(Regex("\\s+"), " ").trim()
            if (text.isNotBlank() && normalized != previous) {
                lastText = text
                handler.post { output?.text = "Распознано. Перевожу…\n$text" }
                executor.execute {
                    val translated = runCatching { TranslationProvider.translate(this@TranslatorOverlayService, text) }.getOrElse { "Ошибка перевода: " + (it.message ?: "API") }
                    handler.post {
                        lastTranslation = translated
                        output?.text = "ОРИГИНАЛ\n$text\n\nПЕРЕВОД\n$translated"
                    }
                }
            }
        }.addOnFailureListener { e -> handler.post { output?.text = "OCR: " + (e.message ?: "ошибка") } }
            .addOnCompleteListener { image.close() }
    }

    private fun createOverlay() {
        windowManager = getSystemService(WINDOW_SERVICE) as WindowManager
        val container = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(12, 8, 12, 8); setBackgroundColor(0xDD111923.toInt()) }
        val toggle = Button(this).apply { setText("ПЕРЕВОД"); setOnClickListener { panel?.visibility = if (panel?.visibility == View.VISIBLE) View.GONE else View.VISIBLE } }
        val text = TextView(this).apply { setTextColor(0xFFFFFFFF.toInt()); textSize = 13f; this.text = "WinTranslator: ожидание текста"; setPadding(8, 8, 8, 8) }
        output = text
        val actions = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        val pauseButton = Button(this).apply { setText("Пауза") }
        pauseButton.setOnClickListener {
            capturing = !capturing
            pauseButton.setText(if (capturing) "Пауза" else "Продолжить")
        }
        actions.addView(pauseButton)
        actions.addView(Button(this).apply { setText("Скан"); setOnClickListener { lastFrame = 0L; output?.text = "Повторное сканирование…" } })
        actions.addView(Button(this).apply { setText("Настройки"); setOnClickListener { startActivity(Intent(this@TranslatorOverlayService, TranslatorConfigActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) } })
        actions.addView(Button(this).apply { setText("Скрыть"); setOnClickListener { panel?.visibility = View.GONE } })
        actions.addView(Button(this).apply { setText("Стоп"); setOnClickListener { stopSelf() } })
        val details = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; addView(text); addView(actions) }
        panel = details; details.visibility = View.VISIBLE
        container.addView(toggle); container.addView(details); root = container
        val type = if (Build.VERSION.SDK_INT >= 26) WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY else @Suppress("DEPRECATION") WindowManager.LayoutParams.TYPE_PHONE
        val params = WindowManager.LayoutParams(WindowManager.LayoutParams.WRAP_CONTENT, WindowManager.LayoutParams.WRAP_CONTENT, type,
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN, PixelFormat.TRANSLUCENT)
        params.gravity = Gravity.TOP or Gravity.END; params.x = 18; params.y = 160
        runCatching { windowManager?.addView(container, params) }
    }

    override fun onDestroy() {
        runCatching { display?.release() }; runCatching { reader?.close() }; runCatching { projection?.stop() }
        runCatching { root?.let { windowManager?.removeView(it) } }; runCatching { recognizer.close() }
        executor.shutdownNow(); thread.quitSafely(); stopForeground(STOP_FOREGROUND_REMOVE); super.onDestroy()
    }
    override fun onBind(intent: Intent?): IBinder? = null

    companion object {
        private const val CHANNEL = "wintranslator"
        private const val NOTIFICATION_ID = 7301
        private const val EXTRA_CODE = "result_code"
        private const val EXTRA_DATA = "projection_data"
        private const val ACTION_STOP = "com.winlator.cmod.translator.STOP"
        private const val ACTION_PAUSE = "com.winlator.cmod.translator.PAUSE"
        fun start(context: Context, code: Int, data: Intent) {
            val intent = Intent(context, TranslatorOverlayService::class.java).putExtra(EXTRA_CODE, code).putExtra(EXTRA_DATA, data)
            if (Build.VERSION.SDK_INT >= 26) context.startForegroundService(intent) else context.startService(intent)
        }
    }
}
