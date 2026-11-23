package oz.n2kdran

import android.app.Activity
import android.graphics.Color
import android.graphics.Typeface
import android.os.Bundle
import android.os.SystemClock
import android.view.GestureDetector
import android.view.MotionEvent
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager
import android.widget.FrameLayout
import android.widget.TextView
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import java.util.Locale
import kotlin.math.abs

/**
 * Écran principal (DashboardView). Appui long : mode nuit (luminosité minimale). Double appui :
 * écran de mise au point (toutes les données du cœur en texte). Appui sur les
 * indicateurs de connexion (coin bas gauche) : réglages. Triple appui sur
 * une case : choix de la donnée affichée.
 */
class MainActivity : Activity() {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private val core: N2kCore get() = Engine.core
    private lateinit var settings: Settings
    private var settingsOpened = false
    private lateinit var text: TextView
    private lateinit var debugView: android.widget.ScrollView
    private lateinit var dash: DashboardView
    private lateinit var gestures: GestureDetector
    private var renderJob: kotlinx.coroutines.Job? = null
    private var navRate = 0.0
    private var navRateN = 0L
    private var navRateT = 0L

    // Appuis successifs : 1 = réglages (indicateurs), 2 = mise au point, 3 = case.
    private val handler = android.os.Handler(android.os.Looper.getMainLooper())
    private var tapCount = 0
    private var tapX = 0f
    private var tapY = 0f
    private val tapAction = Runnable {
        val n = tapCount
        tapCount = 0
        when {
            n >= 3 -> dash.cellAt(tapX, tapY)?.let { (col, idx) -> chooseCell(col, idx) }
            n == 2 -> debugView.visibility = if (debugView.visibility == android.view.View.VISIBLE)
                android.view.View.GONE else android.view.View.VISIBLE
            dash.aisAlertAt(tapX, tapY) != null ->             // 1er appui : acquitte ; 2e : cible suivante
                dash.aisAlertAt(tapX, tapY)?.let { Engine.aisTap(it) }
            dash.isSailButton(tapX, tapY) -> {                 // sail chart du centre
                settings.sailRose = !settings.sailRose
                dash.sailRose = settings.sailRose
            }
            dash.isStatusArea(tapX, tapY) -> {
                settingsOpened = true
                startActivity(android.content.Intent(this, SettingsActivity::class.java))
            }
        }
    }

    private fun registerTap(x: Float, y: Float) {
        if (tapCount > 0 && (abs(x - tapX) > TAP_SLOP || abs(y - tapY) > TAP_SLOP)) tapCount = 0
        if (tapCount == 0) { tapX = x; tapY = y }
        tapCount++
        handler.removeCallbacks(tapAction)
        handler.postDelayed(tapAction, TAP_WINDOW_MS)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        settings = Settings(this)
        // Mise au point : adb shell am start ... --es host 127.0.0.1 (avec adb reverse).
        Engine.hostOverride = intent.getStringExtra("host")
        startEngine()
        if (checkSelfPermission(android.Manifest.permission.POST_NOTIFICATIONS) !=
            android.content.pm.PackageManager.PERMISSION_GRANTED)
            requestPermissions(arrayOf(android.Manifest.permission.POST_NOTIFICATIONS), 1)
        dash = DashboardView(this, core)
        text = TextView(this).apply {
            typeface = Typeface.MONOSPACE
            setTextColor(Color.WHITE)
            setBackgroundColor(Color.BLACK)
            setPadding(24, 16, 24, 16)
        }
        // Mise au point : défilable (le texte peut dépasser sur un petit écran).
        debugView = android.widget.ScrollView(this).apply {
            setBackgroundColor(Color.BLACK)
            addView(text)
            visibility = android.view.View.GONE
        }
        setContentView(FrameLayout(this).apply {
            addView(dash)
            addView(debugView)
        })
        hideBars(window)
        gestures = GestureDetector(this, object : GestureDetector.SimpleOnGestureListener() {
            override fun onDown(e: MotionEvent) = true
            override fun onLongPress(e: MotionEvent) { tapCount = 0; setNight(!night) }
            override fun onSingleTapUp(e: MotionEvent): Boolean { registerTap(e.x, e.y); return true }
        })
        gestures.setOnDoubleTapListener(null)     // appuis comptés par registerTap()
    }

    override fun dispatchTouchEvent(ev: MotionEvent): Boolean {
        gestures.onTouchEvent(ev)
        // L'écran de mise au point défile au doigt ; l'écran principal ne reçoit rien.
        if (debugView.visibility == android.view.View.VISIBLE) super.dispatchTouchEvent(ev)
        return true
    }

    /** Démarre le service de premier plan et le moteur (sans effet s'ils tournent). */
    private fun startEngine() {
        Engine.onStopped = { runOnUiThread { finish() } }      // « Arrêter » dans la notification
        startForegroundService(android.content.Intent(this, N2kService::class.java))
        Engine.start(this)
    }

    override fun onStart() {
        super.onStart()
        if (!Engine.running) startEngine()
        if (settingsOpened) {                                  // retour des réglages
            settingsOpened = false
            Engine.applySettings()
        }
        applyDisplaySettings()
        renderJob = scope.launch {
            while (isActive) {
                render()
                delay(200)
            }
        }
    }

    /** Choix de la donnée affichée dans une case. */
    private fun chooseCell(col: Int, idx: Int) {
        val defs = Cells.ALL
        val labels = defs.map { d -> if (d.unit.isEmpty()) d.label else "${d.label} (${d.unit})" }.toTypedArray()
        val current = defs.indexOfFirst { it.key == settings.column(col)[idx] }
        val dlg = android.app.AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
            .setTitle(if (col == 2) "Case ${idx + 1} (colonne WP)" else "Case ${idx + 1}")
            .setSingleChoiceItems(labels, current) { d, which ->
                settings.setCell(col, idx, defs[which].key)
                dash.columns = arrayOf(settings.column(0), settings.column(1), settings.column(2))
                dash.invalidate()
                d.dismiss()
            }
            .setNegativeButton("Annuler", null)
            .create()
        // Plein écran conservé : la fenêtre du dialogue ne prend le focus qu'après
        // avoir masqué les barres (sinon la barre des tâches réapparaît).
        dlg.window?.setFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE,
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE)
        dlg.show()
        dlg.window?.let { hideBars(it); it.clearFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE) }
    }

    /** Mode nuit : luminosité de l'écran au minimum (fenêtre de l'app seulement)
     * et voile noir semi-transparent réglable (la SP10X reste trop lumineuse au
     * minimum), mêmes couleurs que le jour ; mode jour : tout rendu au système. */
    private var night = false

    private fun setNight(on: Boolean) {
        night = on
        dash.dim = if (on) settings.nightDimPct / 100f else 0f
        window.attributes = window.attributes.apply {
            screenBrightness = if (on) NIGHT_BRIGHTNESS
            else WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_NONE
        }
    }

    private fun hideBars(w: android.view.Window) {
        w.insetsController?.let {
            it.hide(WindowInsets.Type.systemBars())
            it.systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) hideBars(window)
    }

    /** Réglages d'affichage (les connexions et calculs sont gérés par le moteur). */
    private fun applyDisplaySettings() {
        dash.columns = arrayOf(settings.column(0), settings.column(1), settings.column(2))
        dash.headingRef = settings.headingRef
        dash.sailRose = settings.sailRose
        dash.transducerDepth = settings.transducerDepth
        dash.aisCpaNm = settings.aisCpaNm
        dash.aisTcpaMin = settings.aisTcpaMin.toDouble()
        dash.aisRangeNm = settings.aisRangeNm
        if (night) dash.dim = settings.nightDimPct / 100f
    }

    override fun onStop() {
        // Le moteur continue en arrière-plan (service) : seul l'affichage s'arrête.
        renderJob?.cancel()
        super.onStop()
    }

    override fun onDestroy() {
        scope.cancel()
        if (Engine.onStopped != null) Engine.onStopped = null
        super.onDestroy()
    }

    private fun render() {
        val now = SystemClock.elapsedRealtime()
        core.snapshot(now)
        dash.link = Engine.link
        dash.navLast = { Engine.navLastMs() }
        dash.invalidate()
        if (debugView.visibility != android.view.View.VISIBLE) return
        val st = core.stats()
        val sb = StringBuilder()
        val l = Engine.link
        val age = if (l == null || l.lastLineMs == 0L) "—" else "${now - l.lastLineMs} ms"
        sb.append("n2kdran ${Engine.host}:${Engine.port}  ${l?.state}  dernière ligne $age")
        l?.lastError?.let { sb.append("  ($it)") }
        sb.append("\nlignes ${st[0]}  rejetées ${st[1]}  fp ${st[2]}/${st[3]}  ${Engine.polarMsg}  ${Engine.sailMsg}")
        Engine.navUdp?.let { n ->                      // débit qtVlm (≈ 15 datagrammes/s émis)
            if (now - navRateT >= 5000) {
                navRate = if (navRateT == 0L) 0.0 else (n.packets - navRateN) * 1000.0 / (now - navRateT)
                navRateN = n.packets; navRateT = now
            }
            sb.append(String.format(Locale.ROOT, "\nqtVlm UDP : %d datagrammes, %.1f/s ; relayés %d", n.packets, navRate, n.relayed))
            n.lastError?.let { sb.append("  ($it)") }
        }
        sb.append("\n\n")

        // Trois colonnes ; taille du texte ajustée à la largeur de l'écran.
        val rows = core.names.indices.filter { core.status[it] != N2kCore.NEVER }
        val third = (rows.size + 2) / 3
        for (r in 0 until third) {
            for (c in 0 until 3) {
                val k = r + c * third
                if (k < rows.size) sb.append(if (c > 0) "  " else "").append(cell(rows[k]))
            }
            sb.append('\n')
        }
        val w = text.width - text.paddingLeft - text.paddingRight
        if (w > 0) {
            val px = w / (CELL_CHARS * 3 + 4) / 0.6f          // chasse ≈ 0,6 × taille
            text.setTextSize(android.util.TypedValue.COMPLEX_UNIT_PX, px.coerceAtMost(40f))
        }
        text.text = sb
    }

    private fun cell(i: Int): String {
        val name = core.names[i]
        val (k, off, unit) = display(name)
        val v = core.values[i] * k + off
        val s = if (core.status[i] == N2kCore.OK) String.format(Locale.ROOT, "%9.2f", v) else "      ---"
        return String.format(Locale.ROOT, "%-16s%s %-5s", name.take(16), s, unit)
    }

    private fun display(name: String): Triple<Double, Double, String> {
        val deg = 180.0 / Math.PI
        val kn = 3600.0 / 1852.0
        return when {
            name == "LAT" || name == "LON" -> Triple(1.0, 0.0, "°")
            name == "ROT" -> Triple(deg * 60, 0.0, "°/min")
            name in setOf("SOG", "BSP", "AWS", "TWS", "GWS", "DRIFT", "WCV", "TWS_AVG", "VMG",
                "POLAR_BSP", "TARGET_BSP") -> Triple(kn, 0.0, "kn")
            name in setOf("LOG", "TRIP", "DTW", "DTW_SAIL") -> Triple(1 / 1852.0, 0.0, "NM")
            name.startsWith("TTW") || name.startsWith("TTL") -> Triple(1 / 60.0, 0.0, "min")
            name.endsWith("_TEMP") -> Triple(1.0, -273.15, "°C")
            name == "PRESSURE" -> Triple(0.01, 0.0, "hPa")
            name.endsWith("_PCT") || name == "HUMIDITY" -> Triple(1.0, 0.0, "%")
            name in setOf("DEPTH", "DEPTH_OFFSET", "XTE") -> Triple(1.0, 0.0, "m")
            name in setOf("GNSS_METHOD", "GNSS_SATS", "HDOP", "PDOP") -> Triple(1.0, 0.0, "")
            else -> Triple(deg, 0.0, "°")          // angles
        }
    }


    companion object {
        private const val CELL_CHARS = 31             // 16 + 9 + 1 + 5
        private const val TAP_WINDOW_MS = 350L    // délai max entre deux appuis
        private const val TAP_SLOP = 80f          // px : appuis au même endroit
        private const val NIGHT_BRIGHTNESS = 0.01f // luminosité minimale (0 = valeur interdite)
    }
}
