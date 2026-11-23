package oz.n2kdran

import android.content.Context
import android.graphics.Canvas
import android.graphics.LinearGradient
import android.graphics.Paint
import android.graphics.Path
import android.graphics.PorterDuff
import android.graphics.PorterDuffXfermode
import android.graphics.RectF
import android.graphics.Shader
import android.graphics.Typeface
import android.os.SystemClock
import android.view.View
import java.util.Locale
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.hypot
import kotlin.math.min
import kotlin.math.roundToInt
import kotlin.math.sign
import kotlin.math.sin

/**
 * Écran principal, dessiné dans un repère virtuel de 1280 × 800 mis à l'échelle :
 * colonne de 5 cases à gauche, rose de 800 × 800 au centre (cap en haut),
 * colonne de 5 cases à droite. Toute donnée non fraîche est grisée / « --- ».
 */
class DashboardView(context: Context, private val core: N2kCore) :
    View(context) {

    /** Connexion o3nav (état affiché) et date du dernier paquet de navigation. */
    var link: YdrawLink? = null
    var navLast: () -> Long = { 0L }

    /** Référence du cap affiché : 'V' vrai, 'M' magnétique, 'C' COG (route fond en haut). */
    var headingRef = 'V'
    /** Immersion du transducteur (m), ajoutée à la profondeur si le sondeur n'envoie pas de décalage positif. */
    var transducerDepth = 0.0
        set(value) { field = value; invalidate() }

    /** Seuils de la cible AIS dangereuse : CPA (NM) et TCPA (min). */
    var aisCpaNm = 0.5
    var aisTcpaMin = 20.0
    var aisRangeNm = Settings.DEFAULT_AIS_RANGE
    private var aisThreat: N2kCore.AisTarget? = null
    private var aisDistress: N2kCore.AisTarget? = null
    /** Homme à la mer en cours : les autres alertes AIS sont coupées. */
    private var aisMob = false
    /** Hommes à la mer en cours (MOB d'abord dans le cœur). */
    private var aisMobs: List<N2kCore.AisTarget> = emptyList()

    /** Voile noir semi-transparent par-dessus tout l'écran (0 = aucun, 1 = noir). */
    var dim = 0f
        set(value) { field = value.coerceIn(0f, 0.95f); invalidate() }

    // --- Palette ---------------------------------------------------------------
    private class Palette(
        val bg: Int, val fg: Int, val label: Int, val stale: Int, val grid: Int,
        val stbd: Int, val port: Int, val ok: Int, val bad: Int,
    )

    private val day = Palette(
        bg = 0xFF000000.toInt(), fg = 0xFFFFFFFF.toInt(), label = 0xFFB0B0B0.toInt(),
        stale = 0xFF505050.toInt(), grid = 0xFF303030.toInt(),
        stbd = 0xFF2ECC40.toInt(), port = 0xFFFF4136.toInt(),
        ok = 0xFF2ECC40.toInt(), bad = 0xFFFF4136.toInt(),
    )
    private val p = day

    // --- Données ---------------------------------------------------------------
    private fun ix(n: String) = core.idx(n)
    private fun v(i: Int): Double? =
        if (i >= 0 && core.status[i] == N2kCore.OK) core.values[i] else null

    private val iVar = ix("VARIATION")
    private val iHdg = ix("HDG_TRUE"); private val iCog = ix("COG")
    private val iTwd = ix("TWD"); private val iTwa = ix("TWA"); private val iAwa = ix("AWA")
    private val iSet = ix("SET"); private val iDrift = ix("DRIFT"); private val iBtw = ix("BTW")
    private val iTgt = ix("TARGET_TWA")
    private val iTwaPct = ix("TWA_VMG_PCT")
    private val iRudder = ix("RUDDER")
    private val iLs = ix("LAY_STBD_GND"); private val iLp = ix("LAY_PORT_GND")
    private val iLsMin = ix("LAY_STBD_GND_MIN"); private val iLsMax = ix("LAY_STBD_GND_MAX")
    private val iLpMin = ix("LAY_PORT_GND_MIN"); private val iLpMax = ix("LAY_PORT_GND_MAX")
    private val iLsLo = ix("LAY_STBD_GND_LO"); private val iLsHi = ix("LAY_STBD_GND_HI")
    private val iLpLo = ix("LAY_PORT_GND_LO"); private val iLpHi = ix("LAY_PORT_GND_HI")
    private val iOppTwa = ix("OPP_TWA_GND")
    private val iTwdAvg = ix("TWD_AVG")
    private val iDepthOff = ix("DEPTH_OFFSET")

    // --- Cases -----------------------------------------------------------------
    /** Contenu des colonnes (clés de Cells) : 0 gauche, 1 droite, 2 droite avec WP. */
    var columns: Array<List<String>> = Cells.DEFAULTS.copyOf()
    private val iDtw = ix("DTW")
    private val iTtwS = ix("TTW_STBD")
    private val iTtwP = ix("TTW_PORT")
    private fun wpActive() = v(iDtw) != null && v(iBtw) != null

    // --- Outils de dessin -----------------------------------------------------
    private val fill = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL }
    private val stroke = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE }
    private val text = Paint(Paint.ANTI_ALIAS_FLAG).apply { typeface = Typeface.DEFAULT_BOLD }
    private val oval = RectF()
    private val path = Path()

    private val VW = 1280f
    private val VH = 800f
    private val CX = 640f
    private val CY = 400f
    private val KN = 3600.0 / 1852.0
    private val DEG = 180.0 / PI

    override fun onDraw(canvas: Canvas) {
        canvas.drawColor(p.bg)
        val s = min(width / VW, height / VH)
        canvas.save()
        canvas.translate((width - VW * s) / 2f, (height - VH * s) / 2f)
        canvas.scale(s, s)
        val now = SystemClock.elapsedRealtime()
        // Cibles passées (2e appui sur le point) sautées ; balise passée seule : reste affichée.
        val skip = Engine.aisSkipped.toList()
        aisThreat = core.aisThreat(now, aisCpaNm * 1852.0, aisTcpaMin * 60.0, aisRangeNm * 1852.0, skip)
        // Homme à la mer : bandeau permanent (jusqu'à l'arrêt de l'appli), un point par
        // MOB, autres alertes AIS coupées. Sinon balise passée seule : reste affichée.
        aisMobs = core.aisMobs(now)
        aisMob = aisMobs.isNotEmpty()
        aisDistress = aisMobs.firstOrNull() ?: core.aisDistress(now, skip) ?: core.aisDistress(now)
        drawColumn(canvas, columns[0], 0f)
        drawColumn(canvas, columns[if (wpActive()) 2 else 1], VW - 240f)
        drawRose(canvas)
        aisDistress?.let { if (aisMob || it.mmsi !in Engine.aisAcked) drawDistress(canvas, it) }
        canvas.restore()
        if (dim > 0f) canvas.drawColor(((dim * 255).toInt() shl 24))   // voile de nuit
    }

    // --- Colonnes de cases ----------------------------------------------------
    private fun drawColumn(c: Canvas, keys: List<String>, x0: Float) {
        // Cadre de la colonne et séparations : trait simple de 2 px partout.
        stroke.color = p.grid; stroke.strokeWidth = 2f; stroke.pathEffect = null
        c.drawRect(x0 + 1, 1f, x0 + 239, keys.size * 160f - 1, stroke)
        for (k in 1 until keys.size) c.drawLine(x0, k * 160f, x0 + 240, k * 160f, stroke)
        for ((k, key) in keys.withIndex()) {
            val cell = Cells.get(key)
            val y0 = k * 160f
            val i = ix(cell.item)
            val value = v(i)
            text.textSize = 26f; text.textAlign = Paint.Align.LEFT; text.color = p.label
            c.drawText(cell.label, x0 + 14, y0 + 34, text)
            if (cell.kind == Kind.ANGLE_SIDE && value != null) {   // point d'amure (qtVlm)
                fill.color = if (value >= 0) p.stbd else p.port
                c.drawCircle(x0 + 26 + text.measureText(cell.label), y0 + 25, 9f, fill)
            }
            // XTE (qtVlm : négatif = à droite de la route) : pastille verte à droite
            // de la route (côté tribord), rouge à gauche (côté bâbord).
            if (cell.kind == Kind.XTE && value != null && abs(value) >= 0.5) {
                fill.color = if (value < 0) p.stbd else p.port
                c.drawCircle(x0 + 26 + text.measureText(cell.label), y0 + 25, 9f, fill)
            }
            text.textAlign = Paint.Align.RIGHT
            // XTE : unité selon la valeur (m sous 0,1 NM, NM au-delà).
            val unit = if (cell.kind == Kind.XTE && value != null && abs(value) < 185.2) "m" else cell.unit
            c.drawText(unit, x0 + 226, y0 + 34, text)

            if (cell.kind == Kind.AIS) { drawAisCell(c, x0, y0); continue }
            if (cell.kind == Kind.SAIL) { drawSailCell(c, x0, y0); continue }
            if (cell.kind == Kind.SAILCHART) { drawSailChartCell(c, x0, y0); continue }
            if (cell.kind == Kind.PS) {                      // deux temps : P et S
                text.textSize = 56f; text.textAlign = Paint.Align.LEFT
                val tp = v(iTtwP); val ts = v(iTtwS)
                text.color = if (tp == null) p.stale else p.port
                c.drawText("P " + (tp?.let { fmtTime(it) } ?: "---"), x0 + 16, y0 + 92, text)
                text.color = if (ts == null) p.stale else p.stbd
                c.drawText("S " + (ts?.let { fmtTime(it) } ?: "---"), x0 + 16, y0 + 148, text)
                continue
            }
            var color = p.fg
            val str = if (value == null) {
                color = p.stale; "---"
            } else when (cell.kind) {
                Kind.SPEED -> fmt1(value * KN)
                Kind.SPEED_ABS -> fmt1(abs(value) * KN)
                Kind.PCT -> value.roundToInt().toString()
                // Sous la surface : décalage du sondeur s'il est positif (transducteur → surface),
                // sinon immersion réglée dans n2kdran (un décalage négatif vise la quille).
                Kind.DEPTH -> fmt1(value + (v(iDepthOff)?.takeIf { it > 0 } ?: transducerDepth))
                Kind.DIR -> String.format(Locale.ROOT, "%03d", (norm360(value * DEG)).roundToInt() % 360)
                Kind.ANGLE_SIDE, Kind.ANGLE -> abs(value * DEG).roundToInt().toString()
                Kind.ANGLE1 -> fmt1(abs(value * DEG))
                Kind.SIGNED_ANGLE -> String.format(Locale.ROOT, "%+.0f", value * DEG)
                Kind.TIME -> fmtTime(value)
                Kind.DIST -> (value / 1852.0).let { if (it < 10) String.format(Locale.ROOT, "%.2f", it) else fmt1(it) }
                Kind.TEMP -> fmt1(value - 273.15)
                Kind.PRESS -> (value / 100).roundToInt().toString()
                Kind.NUM1 -> fmt1(value)
                Kind.INT -> value.roundToInt().toString()
                Kind.XTE -> abs(value).let { x ->               // côté dans le libellé
                    if (x < 185.2) x.roundToInt().toString()
                    else (x / 1852.0).let { if (it < 10) String.format(Locale.ROOT, "%.2f", it) else fmt1(it) }
                }
                Kind.PS, Kind.SAIL, Kind.SAILCHART, Kind.AIS -> ""          // dessinées à part
            }
            text.color = color; text.textSize = 104f; text.textAlign = Paint.Align.CENTER
            val w = text.measureText(str)
            if (w > 216f) text.textSize = 104f * 216f / w     // ajuste à la case
            c.drawText(str, x0 + 120, y0 + 138, text)
        }
    }

    // --- AIS ------------------------------------------------------------------

    /** Distance en NM : 2 décimales sous 10 NM. */

    /** Distance AIS avec son unité : au mètre sous 1 000 m, sinon en milles. */
    private fun fmtDist(m: Double) = when {
        m < 1000 -> "${m.roundToInt()} m"
        m < 10 * 1852 -> String.format(Locale.ROOT, "%.2f NM", m / 1852.0)
        else -> fmt1(m / 1852.0) + " NM"
    }

    /** Case AIS : cible la plus menaçante, nom (ou MMSI) en haut à droite, CPA et
     *  TCPA ; en rouge si CPA et TCPA sont sous les seuils des réglages. */
    private fun drawAisCell(c: Canvas, x0: Float, y0: Float) {
        val t = aisThreat
        if (t == null) {
            text.color = p.stale; text.textSize = 104f; text.textAlign = Paint.Align.CENTER
            c.drawText("---", x0 + 120, y0 + 138, text)
            return
        }
        val id = t.name ?: t.mmsi.toString()
        text.textSize = 24f; text.textAlign = Paint.Align.RIGHT; text.color = p.label
        val maxW = 150f
        val w = text.measureText(id)
        if (w > maxW) text.textSize = 24f * maxW / w
        c.drawText(id, x0 + 226, y0 + 34, text)
        text.color = if (t.level == 2) p.bad else p.fg
        val min = t.tcpa / 60.0
        for ((str, y) in listOf(fmtDist(t.cpa) to 92f,
                                (if (min < 1) "< 1 min" else "${min.roundToInt()} min") to 148f)) {
            text.textSize = 56f; text.textAlign = Paint.Align.LEFT
            val sw = text.measureText(str)
            if (sw > 208f) text.textSize = 56f * 208f / sw     // ajuste à la case
            c.drawText(str, x0 + 16, y0 + y, text)
        }
    }

    /** Alerte balise de détresse AIS (SART, MOB, EPIRB) : bandeau clignotant au centre. */
    private fun drawDistress(c: Canvas, t: N2kCore.AisTarget) {
        // Clignote tant que l'alerte sonne ; MOB acquitté : rouge fixe, toujours affiché.
        val ringing = if (aisMob) aisMobs.any { it.mmsi !in Engine.aisAcked } else t.mmsi !in Engine.aisAcked
        val on = !ringing || (SystemClock.elapsedRealtime() / 500) % 2 == 0L
        fill.color = if (on) 0xFFE00000.toInt() else 0xFF700000.toInt()
        c.drawRoundRect(BAN_L, BAN_T, BAN_R, BAN_B, 24f, 24f, fill)
        stroke.color = 0xFFFFFFFF.toInt(); stroke.strokeWidth = 4f; stroke.pathEffect = null
        c.drawRoundRect(BAN_L, BAN_T, BAN_R, BAN_B, 24f, 24f, stroke)
        val kind = when (t.level) { 972 -> "MOB AIS"; 974 -> "EPIRB AIS"; N2kCore.MOB_N2K -> "HOMME À LA MER"; else -> "AIS-SART" }
        text.color = 0xFFFFFFFF.toInt(); text.textAlign = Paint.Align.CENTER
        fitText(kind, 64f); c.drawText(kind, CX, 360f, text)
        val who = t.name ?: if (t.level == N2kCore.MOB_N2K) "alerte MOB du bus N2K" else t.mmsi.toString()
        fitText(who, 40f); c.drawText(who, CX, 418f, text)
        // Position estimée (celle du bateau à la réception) : signalée inconnue, direction approchée.
        val where = if (noDir(t)) "position inconnue"
            else (if (t.est) "position inconnue  ≈ " else "") + fmtDist(t.range) + "  " +
                String.format(Locale.ROOT, "%03d°", norm360(t.bearing * DEG).roundToInt() % 360)
        fitText(where, 52f); c.drawText(where, CX, 482f, text)
    }

    /** Bandeau de détresse : coins à l'intérieur de la carte, sans mordre sur le
     *  cercle des allures où sont posés les points des cibles. */
    private val BAN_L = 310f; private val BAN_R = 970f
    private val BAN_T = 290f; private val BAN_B = 510f

    /** Taille de `text` réduite si `s` dépasse la largeur utile du bandeau. */
    private fun fitText(s: String, size: Float) {
        text.textSize = size
        val w = text.measureText(s); val max = BAN_R - BAN_L - 40f
        if (w > max) text.textSize = size * max / w
    }

    // --- Voilure (sail chart qtVlm) ---------------------------------------------

    /** Case VOILE : nom de la configuration recommandée sur sa couleur qtVlm. */
    private fun drawSailCell(c: Canvas, x0: Float, y0: Float) {
        val chart = Engine.sailChart
        val code = Engine.sailCode
        val name = chart?.name(code)
        if (chart == null || code == null || name == null) {
            text.color = p.stale; text.textSize = 104f; text.textAlign = Paint.Align.CENTER
            c.drawText("---", x0 + 120, y0 + 138, text)
            return
        }
        if (chart.isNone(code)) {                    // pas de voile recommandée à cet angle
            text.color = p.label; text.textSize = 70f; text.textAlign = Paint.Align.CENTER
            c.drawText("N/A", x0 + 120, y0 + 99 - (text.descent() + text.ascent()) / 2, text)
            return
        }
        val col = chart.color(code)
        fill.color = col
        c.drawRoundRect(x0 + 10, y0 + 48, x0 + 230, y0 + 150, 14f, 14f, fill)
        text.color = if (SailChart.luminance(col) > 0.55) 0xFF000000.toInt() else 0xFFFFFFFF.toInt()
        text.textSize = 70f; text.textAlign = Paint.Align.CENTER
        val w = text.measureText(name)
        if (w > 200f) text.textSize = 70f * 200f / w
        c.drawText(name, x0 + 120, y0 + 99 - (text.descent() + text.ascent()) / 2, text)
    }

    private var chartBitmap: android.graphics.Bitmap? = null
    private var chartBitmapOf: SailChart? = null
    private val CH_W = 220
    private val CH_H = 106
    private val CH_TWA0 = 40.0
    private val CH_TWA1 = 180.0

    /** Case SAIL (sail chart) : grille TWA (40–180°) × TWS colorée, croix à la position actuelle. */
    private fun drawSailChartCell(c: Canvas, x0: Float, y0: Float) {
        val chart = Engine.sailChart ?: run {
            text.color = p.stale; text.textSize = 104f; text.textAlign = Paint.Align.CENTER
            c.drawText("---", x0 + 120, y0 + 138, text); return
        }
        val twsMax = chart.tws.lastOrNull() ?: 30.0
        if (chartBitmapOf !== chart) {                    // fond calculé une fois par sail chart
            val bmp = android.graphics.Bitmap.createBitmap(CH_W, CH_H, android.graphics.Bitmap.Config.ARGB_8888)
            for (x in 0 until CH_W) for (y in 0 until CH_H) {
                val twa = CH_TWA0 + (x + 0.5) / CH_W * (CH_TWA1 - CH_TWA0)
                val tws = twsMax * (1 - (y + 0.5) / CH_H)
                val code = chart.codeAt(twa, tws)
                bmp.setPixel(x, y, if (code == null || chart.isNone(code)) 0xFF000000.toInt() else chart.color(code))
            }
            chartBitmap = bmp
            chartBitmapOf = chart
        }
        val left = x0 + 10; val top = y0 + 46
        chartBitmap?.let { c.drawBitmap(it, left, top, null) }
        stroke.color = p.grid; stroke.strokeWidth = 1f; stroke.pathEffect = null
        c.drawRect(left, top, left + CH_W, top + CH_H, stroke)
        // Nom de la configuration retenue, en haut à droite, dans sa couleur.
        val code = Engine.sailCode
        chart.name(code)?.takeIf { code != null && !chart.isNone(code) }?.let {
            text.textSize = 24f; text.textAlign = Paint.Align.RIGHT; text.color = chart.color(code)
            c.drawText(it, x0 + 228, y0 + 34, text)
        }
        // Croix à la position actuelle (|TWA|, TWS).
        val twa = v(iTwa) ?: return
        val tws = v(ix("TWS")) ?: return
        val a = Math.toDegrees(abs(twa)).coerceIn(CH_TWA0, CH_TWA1)
        val k = (tws * KN).coerceIn(0.0, twsMax)
        val cx = left + ((a - CH_TWA0) / (CH_TWA1 - CH_TWA0) * CH_W).toFloat()
        val cy = top + ((1 - k / twsMax) * CH_H).toFloat()
        for ((w, col) in listOf(6f to 0xFF000000.toInt(), 3f to 0xFFFFFFFF.toInt())) {
            stroke.strokeWidth = w; stroke.color = col
            c.drawLine(cx - 9, cy - 9, cx + 9, cy + 9, stroke)
            c.drawLine(cx - 9, cy + 9, cx + 9, cy - 9, stroke)
        }
    }

    /** Durée : « m:ss » sous l'heure, « h:mm » au-delà (suffixe h). */
    private fun fmtTime(sec: Double): String {
        val t = sec.roundToInt().coerceAtLeast(0)
        return if (t < 3600) String.format(Locale.ROOT, "%d:%02d", t / 60, t % 60)
        else String.format(Locale.ROOT, "%dh%02d", t / 3600, (t % 3600) / 60)
    }

    private fun fmt1(x: Double) = String.format(Locale.ROOT, "%.1f", x)
    private fun norm360(d: Double) = ((d % 360) + 360) % 360

    // --- Rose -----------------------------------------------------------------
    // Tout est dessiné ici (pas d'images reprises d'un autre instrument), sauf la
    // silhouette du CM50. Repère 1080 hérité : rose de rayon 540 centrée en (540, 540).
    private fun img(id: Int) = context.getDrawable(id)!!.mutate()
    private val dBoat = img(R.drawable.cm50_outline)     // silhouette du CM50 (319×625, en (380, 225) dans le repère 1080)

    private val K = 800f / 1080f            // repère 1080 → repère virtuel
    // Cercle des allures tout à l'extérieur (515–540), carte des angles juste à l'intérieur
    // (381–512 × CARD_S…), triangles TWA/AWA/WP pointe vers l'extérieur sur le cercle médian.
    private val CARD_S = 512f / 437f                 // historique : facteur de la carte agrandie
    private val R_CARD_IN = 381f * CARD_S * K         // intérieur de la rose (laylines, sail chart…)
    private val R_CARD_OUT = 512f * K                // bord extérieur de la carte
    private val R_GAP_IN = 515f * K                  // cercle des allures
    private val R_GAP_OUT = 540f * K
    /** Décalage vers le haut du cadre du cap, centré sur la carte (repère 1080). */
    private val HDG_DY = -(409f * CARD_S - 406f)

    /** Carte des angles, tournée de `a` (rad) : anneau blanc, graduations tous les 5°,
     *  cardinaux en lettres, autres dizaines sans zéro de tête (30, 60…). */
    private fun drawCard(c: Canvas, a: Double) {
        val ink = 0xFF000000.toInt()
        c.save(); c.rotate((a * DEG).toFloat(), CX, CY)
        stroke.pathEffect = null
        // Cible AIS dangereuse : la couronne pulse du blanc au rouge (période 1 s).
        val ring = if (!aisMob && aisThreat?.let { it.level == 2 && it.mmsi !in Engine.aisAcked } == true) {
            val t = (1 - kotlin.math.cos(2 * PI * (SystemClock.elapsedRealtime() % 1000) / 1000.0)) / 2
            lerpColor(0xFFF4F4F4.toInt(), 0xFFFF2020.toInt(), t)
        } else 0xFFF4F4F4.toInt()
        stroke.color = ring; stroke.strokeWidth = R_CARD_OUT - R_CARD_IN
        val rm = (R_CARD_IN + R_CARD_OUT) / 2
        c.drawCircle(CX, CY, rm, stroke)
        stroke.color = 0xFF606060.toInt(); stroke.strokeWidth = 1.5f
        c.drawCircle(CX, CY, R_CARD_IN, stroke); c.drawCircle(CX, CY, R_CARD_OUT, stroke)
        stroke.strokeCap = Paint.Cap.ROUND
        for (d in 0 until 360 step 5) {
            if (d % 30 == 0) continue                          // place des inscriptions
            val t = d / DEG
            val long = d % 10 == 0
            stroke.strokeWidth = if (long) 3f else 2f
            stroke.color = if (long) 0xFF404040.toInt() else 0xFF909090.toInt()
            val r1 = R_CARD_OUT - 4f; val r0 = r1 - if (long) 16f else 9f
            c.drawLine(px(t, r0), py(t, r0), px(t, r1), py(t, r1), stroke)
        }
        stroke.strokeCap = Paint.Cap.BUTT
        text.textAlign = Paint.Align.CENTER; text.color = ink
        for (d in 0 until 360 step 30) {
            val s = when (d) { 0 -> "N"; 90 -> "E"; 180 -> "S"; 270 -> "W"; else -> d.toString() }
            text.textSize = if (d % 90 == 0) 34f else 28f
            c.save(); c.rotate(d.toFloat(), CX, CY)
            c.drawText(s, CX, CY - R_DOTS + text.textSize * 0.36f, text)
            c.restore()
        }
        c.restore()
    }

    /** COG : triangle fin magenta (#B2458A) en travers de la carte, base à l'extérieur,
     *  pointe vers le centre (angle écran `a`). */
    private fun drawCog(c: Canvas, a: Double) {
        c.save(); c.rotate((a * DEG).toFloat(), CX, CY)
        // Déborde du cadre du cap (rayons 326–384) en haut et en bas pour rester visible derrière.
        val r1 = (R_GAP_IN + R_GAP_OUT) / 2; val r0 = R_CARD_IN - 32f; val half = 8f   // base à mi-hauteur des allures
        path.reset()
        path.moveTo(CX - half, CY - r1); path.lineTo(CX + half, CY - r1); path.lineTo(CX, CY - r0); path.close()
        fill.color = 0xFFB2458A.toInt(); c.drawPath(path, fill)
        stroke.color = 0xFF000000.toInt(); stroke.strokeWidth = 1.5f; stroke.pathEffect = null
        stroke.strokeJoin = Paint.Join.ROUND; c.drawPath(path, stroke); stroke.strokeJoin = Paint.Join.MITER
        c.restore()
    }

    /** Cadre du cap, fixe en haut (repère 1080 : x 475–605, y 95–172 avant décalage). */
    private fun drawHdgBox(c: Canvas) {
        val top = CY - 400 + (95f + HDG_DY) * K; val bot = CY - 400 + (172f + HDG_DY) * K
        val l = CX - 65f * K; val r = CX + 65f * K
        fill.color = 0xFFFFFFFF.toInt(); c.drawRoundRect(l, top, r, bot, 10f, 10f, fill)
        stroke.color = 0xFF000000.toInt(); stroke.strokeWidth = 3f; stroke.pathEffect = null
        c.drawRoundRect(l, top, r, bot, 10f, 10f, stroke)
    }

    private val WP_SEE_THROUGH = 75        // opacité (0–255) de la pointe à travers le cadre

    /** Rayon du cercle médian de la carte (pointes des triangles). */
    private val R_DOTS = 409f * CARD_S * K

    /** Triangles TWA/AWA : pointe vers l'extérieur sur le cercle médian, base vers le centre
     *  (repère 1080 : hauteur 99,7, base 92). */
    private val NEEDLE_TIP = 540f - 409f * CARD_S
    private val NEEDLE_BASE = NEEDLE_TIP + 99.7f
    private fun drawNeedle(c: Canvas, a: Double, col: Int, letter: String) {
        val tip = CY - R_DOTS; val base = tip + 99.7f * K; val half = 46f * K
        c.save(); c.rotate((a * DEG).toFloat(), CX, CY)
        path.reset()
        path.moveTo(CX, tip); path.lineTo(CX - half, base); path.lineTo(CX + half, base); path.close()
        fill.color = col; c.drawPath(path, fill)
        stroke.color = 0xFF000000.toInt(); stroke.strokeWidth = 1.5f; stroke.pathEffect = null
        stroke.strokeJoin = Paint.Join.ROUND; c.drawPath(path, stroke); stroke.strokeJoin = Paint.Join.MITER
        text.textSize = 26f; text.textAlign = Paint.Align.CENTER; text.color = 0xFFFFFFFF.toInt()
        text.style = Paint.Style.STROKE; text.strokeWidth = 3f; text.color = 0xFF000000.toInt()
        val ty = base - 38f * K + 9f
        c.drawText(letter, CX, ty, text)
        text.style = Paint.Style.FILL; text.color = 0xFFFFFFFF.toInt()
        c.drawText(letter, CX, ty, text)
        c.restore()
    }

    /** Relèvement du WP : triangle jaune pointe vers l'extérieur, pointe sur le cercle
     *  médian de la carte (angle écran `a`). */
    private fun drawWpMark(c: Canvas, a: Double, alpha: Int = 255) {
        val h = 47f; val half = 20f
        c.save(); c.rotate((a * DEG).toFloat(), CX, CY)
        path.reset()
        path.moveTo(CX, CY - R_DOTS)                         // pointe, vers l'extérieur
        path.lineTo(CX - half, CY - R_DOTS + h)
        path.lineTo(CX + half, CY - R_DOTS + h)
        path.close()
        fill.color = 0xFFFFD228.toInt(); fill.alpha = alpha; c.drawPath(path, fill)
        stroke.color = 0xFF000000.toInt(); stroke.alpha = alpha; stroke.strokeWidth = 1.2f; stroke.pathEffect = null
        c.drawPath(path, stroke)
        c.restore()
    }

    /** Angle écran (rad, 0 = haut, sens horaire) d'un relèvement vrai. */
    private var hdg = 0.0
    private fun sa(bearing: Double) = bearing - hdg
    private fun px(a: Double, r: Float) = (CX + r * sin(a)).toFloat()
    private fun py(a: Double, r: Float) = (CY - r * cos(a)).toFloat()
    private fun arcStart(a: Double) = (a * DEG - 90).toFloat()

    /** Silhouette du bateau, liée au bateau, à sa place dans l'ancienne couronne. */
    private fun drawBoat(c: Canvas, rot: Double) {
        c.save()
        c.rotate((rot * DEG).toFloat(), CX, CY)
        val k = K * CARD_S                           // même agrandissement que la carte, autour du centre
        dBoat.setBounds((CX + (380 - 540) * k).toInt(), (CY + (225 - 540) * k).toInt(),
                        (CX + (699 - 540) * k).toInt(), (CY + (850 - 540) * k).toInt())
        dBoat.draw(c)
        c.restore()
    }

    // --- Sail chart polaire au centre de la rose (essai) ------------------------
    /** Affiche le sail chart en polaire au centre de la rose. */
    var sailRose = true                                   // réglage (MainActivity)
    private var roseBitmap: android.graphics.Bitmap? = null
    private var roseBitmapOf: SailChart? = null
    private val R_SAIL = R_CARD_IN - 4f                   // rayon = TWS maxi du sail chart
    private val ROSE_RES = 2                              // calcul 1 point sur 2, lissé à l'affichage
    private val SAIL_ROSE_ALPHA = 0x80
    private val bmpPaint = Paint(Paint.FILTER_BITMAP_FLAG or Paint.ANTI_ALIAS_FLAG)

    /** Lié au bateau comme le cercle des allures : angle depuis l'étrave = TWA (bâbord et
     *  tribord symétriques), rayon = TWS. Le point actuel est sous le triangle T. */
    private fun drawSailRose(c: Canvas, boatRot: Double) {
        val chart = Engine.sailChart ?: return
        val twsMax = chart.tws.lastOrNull() ?: return
        if (roseBitmapOf !== chart) {
            val n = (2 * R_SAIL / ROSE_RES).toInt()
            val bmp = android.graphics.Bitmap.createBitmap(n, n, android.graphics.Bitmap.Config.ARGB_8888)
            val half = n / 2.0
            for (y in 0 until n) for (x in 0 until n) {
                val dx = x + 0.5 - half; val dy = half - (y + 0.5)
                val r = Math.hypot(dx, dy) / half
                if (r > 1) continue
                val twa = Math.toDegrees(abs(Math.atan2(dx, dy)))
                val code = chart.codeAt(twa, r * twsMax)
                if (code == null || chart.isNone(code)) continue
                bmp.setPixel(x, y, (chart.color(code) and 0xFFFFFF) or (SAIL_ROSE_ALPHA shl 24))
            }
            roseBitmap = bmp; roseBitmapOf = chart
        }
        val bmp = roseBitmap ?: return
        c.save(); c.rotate((boatRot * DEG).toFloat(), CX, CY)
        oval.set(CX - R_SAIL, CY - R_SAIL, CX + R_SAIL, CY + R_SAIL)
        c.drawBitmap(bmp, null, oval, bmpPaint)
        // Cercles de TWS tous les 10 kn.
        stroke.color = 0x60FFFFFF; stroke.strokeWidth = 1f; stroke.pathEffect = null
        var k = 10.0
        while (k < twsMax) { c.drawCircle(CX, CY, (k / twsMax * R_SAIL).toFloat(), stroke); k += 10.0 }
        c.restore()
    }

    /** Position actuelle (TWA, TWS) sur le sail chart du centre : petite croix, dessinée
     *  après la jauge de barre pour rester visible par-dessus. */
    private fun drawSailRoseCross(c: Canvas, boatRot: Double) {
        val chart = Engine.sailChart ?: return
        val twsMax = chart.tws.lastOrNull() ?: return
        val twa = v(iTwa) ?: return
        val tws = v(ix("TWS")) ?: return
        val r = ((tws * KN).coerceIn(0.0, twsMax) / twsMax * R_SAIL).toFloat()
        val a = twa + boatRot
        val x = px(a, r); val y = py(a, r)
        for ((w, col) in listOf(5f to 0xFF000000.toInt(), 2.5f to 0xFFFFFFFF.toInt())) {
            stroke.strokeWidth = w; stroke.color = col
            c.drawLine(x - 8, y - 8, x + 8, y + 8, stroke)
            c.drawLine(x - 8, y + 8, x + 8, y - 8, stroke)
        }
    }

    /** Bouton du sail chart en haut à gauche de la rose, façon bouton poussoir à anneau
     *  lumineux : fond anthracite, anneau vert quand le sail chart du centre est affiché,
     *  rouge quand il est masqué. */
    private val BTN_X = 292f
    private val BTN_Y = 40f
    private val BTN_R = 24f
    private val btnShade = android.graphics.RadialGradient(BTN_X - 6f, BTN_Y - 8f, BTN_R,
        0xFF4A4D52.toInt(), 0xFF2A2C30.toInt(), android.graphics.Shader.TileMode.CLAMP)

    private fun drawSailButton(c: Canvas) {
        if (Engine.sailChart == null) return
        val led = if (sailRose) 0xFF30E030.toInt() else 0xFFE02020.toInt()
        stroke.pathEffect = null
        // Halo de l'anneau (plusieurs traits de plus en plus larges et transparents).
        for ((w, a) in listOf(12f to 0x30, 8f to 0x50)) {
            stroke.color = led; stroke.alpha = a; stroke.strokeWidth = w
            c.drawCircle(BTN_X, BTN_Y, BTN_R - 5f, stroke)
        }
        // Lunette métallique sombre, anneau lumineux, disque anthracite.
        fill.color = 0xFF1A1B1E.toInt(); c.drawCircle(BTN_X, BTN_Y, BTN_R, fill)
        stroke.color = led; stroke.alpha = 255; stroke.strokeWidth = 4f
        c.drawCircle(BTN_X, BTN_Y, BTN_R - 5f, stroke)
        fill.shader = btnShade; c.drawCircle(BTN_X, BTN_Y, BTN_R - 8f, fill); fill.shader = null
    }

    /** Nom de la voile retenue, dans sa couleur, en haut à droite de la rose. */
    private fun drawSailName(c: Canvas) {
        val chart = Engine.sailChart ?: return
        val code = Engine.sailCode ?: return
        val none = chart.isNone(code)                // pas de voile recommandée (ex. TWA < 42° pour CM50)
        val name = if (none) "N/A" else chart.name(code) ?: return
        text.textSize = 34f; text.textAlign = Paint.Align.RIGHT
        text.color = if (none) p.label else chart.color(code)
        c.drawText(name, CX + 392, CY - 400 + 40, text)
    }

    private fun drawRose(c: Canvas) {
        // Référence du cap. Toute la géométrie reste en vrai : « up » est la
        // direction vraie placée en haut ; « label » est la valeur affichée ;
        // « boatRot » fait pivoter ce qui est lié au bateau (mode COG).
        val hT = v(iHdg)
        val cog = v(iCog)
        val variation = v(iVar)
        var up = hT
        var label: Double? = hT
        var boatRot = 0.0
        when (headingRef) {
            'M' -> label = if (hT != null && variation != null) hT - variation else null
            'C' -> {
                up = cog ?: hT
                label = cog
                if (hT != null && cog != null) boatRot = hT - cog
            }
        }
        hdg = up ?: 0.0
        // Carte : un relèvement affiché L est placé à (L − valeur du haut).
        val cardTop = if (headingRef == 'M' && label != null) label else hdg

        // Rose réduite autour de son centre pour ne pas toucher les cases ; les éléments
        // des coins (voile, bouton, indicateurs, nom du WP) restent à leur place.
        val roseLayer = c.save(); c.scale(ROSE_S, ROSE_S, CX, CY)
        if (sailRose) drawSailRose(c, boatRot)       // sous la couronne et le bateau
        drawBoat(c, boatRot)
        drawCard(c, -cardTop)                        // carte : référence en haut

        // Cercle des allures, lié au bateau (0 = étrave) : à chaque angle, couleur du
        // TWA correspondant (même règle que le triangle TWA, qui se pose dessus).
        circleOk = false
        c.save(); c.rotate((boatRot * DEG).toFloat(), CX, CY)
        if (core.vmgProfile(profile)) drawVmgCircle(c)
        drawOptimumTicks(c)
        c.restore()

        drawLaylines(c, cardTop - hdg)
        c.save(); c.rotate((boatRot * DEG).toFloat(), CX, CY)
        drawRudder(c)
        c.restore()
        drawCurrent(c)
        drawWindTrails(c, boatRot)
        if (sailRose) drawSailRoseCross(c, boatRot)  // croix au-dessus des traînées

        cog?.let { drawCog(c, sa(it)) }
        v(iAwa)?.let {                                     // relatif à l'étrave
            drawNeedle(c, it + boatRot, 0xFF4A78B0.toInt(), "A")   // bleu-gris
        }
        v(iTwa)?.let {
            // Couleur du cercle sous le triangle (même lissage), sinon règle directe.
            val col = if (circleOk) circleColorAt(it) else twaColor(it, v(iTwaPct), v(iTgt))
            drawNeedle(c, it + boatRot, col, "T")
        }
        v(iBtw)?.let { drawWpMark(c, sa(it)) }                // par-dessus COG, TWA et AWA

        // Cadre et chiffres du cap (fixes, en haut), lettre de la référence.
        drawHdgBox(c)
        val box = if (label == null) "---" else String.format(Locale.ROOT, "%03d", norm360(label * DEG).roundToInt() % 360)
        text.textSize = 40f; text.textAlign = Paint.Align.CENTER
        text.color = if (label == null) 0xFF808080.toInt() else 0xFF000000.toInt()
        c.drawText(box, CX, CY - 400 + (132f + HDG_DY) * K + 15f, text)
        text.textSize = 15f; text.textAlign = Paint.Align.RIGHT; text.color = 0xFF000000.toInt()
        // Cadre du cap (ic_hdg1080) : x 475–605, y 94–170 dans le repère 1080.
        c.drawText(headingRef.toString(), CX - 400 + 600f * K, CY - 400 + (166f + HDG_DY) * K, text)
        // Sablier COG et pointe du triangle WP vus en transparence à travers le cadre du cap.
        cog?.let { val a = sa(it); if (abs(sin(a)) < 0.2 && cos(a) > 0) {
            // L'alpha d'un VectorDrawable n'est pas appliqué ici : calque translucide.
            val n = c.saveLayerAlpha(null, WP_SEE_THROUGH); drawCog(c, a); c.restoreToCount(n) } }
        v(iBtw)?.let { b ->
            val a = sa(b); if (abs(sin(a)) < 0.2 && cos(a) > 0) drawWpMark(c, a, WP_SEE_THROUGH)
        }
        drawAisDots(c)                               // par-dessus tout le reste de la rose
        c.restoreToCount(roseLayer)

        drawSailName(c)                              // nom de la voile, même sans sail chart au centre
        drawSailButton(c)
        drawStatus(c)
    }

    private val ROSE_S = 0.97f                       // marge d'environ 12 px avec les cases

    /**
     * Couleur du triangle TWA, réglée sur la perte de VMG due à l'angle seul
     * (TWA_VMG_PCT, VMG polaire au TWA actuel / VMG optimum) :
     * vert tant que ≥ 99 % (plage VMG), puis dégradé jusqu'à la couleur franche à 90 %.
     * Côté (logique d'OZDisplay, continue du près au vent arrière) : rouge du côté
     * pincé au près / trop abattu au portant, bleu du côté travers.
     */
    private fun twaColor(twa: Double, pct: Double?, target: Double?): Int {
        if (pct == null || target == null) return 0xFF00FF00.toInt()
        val a = abs(twa)
        val upwind = a < PI / 2
        return vmgColor(pct, upwind == (a < target))
    }

    private fun lerpColor(a: Int, b: Int, t: Double): Int {
        fun ch(x: Int, sh: Int) = (x shr sh) and 0xFF
        fun mix(sh: Int) = (ch(a, sh) + (ch(b, sh) - ch(a, sh)) * t).roundToInt()
        return (0xFF shl 24) or (mix(16) shl 16) or (mix(8) shl 8) or mix(0)
    }

    private val profile = DoubleArray(181)

    private val STEPS = 1440                       // pas de couleur : 0,25°
    private val base = IntArray(STEPS)
    private val smooth = IntArray(STEPS)
    private var circleOk = false
    private val SMOOTH = 12                        // lissage graphique : ±3° (en pas)

    /** % de VMG au TWA t (degrés, 0..180), interpolé entre les degrés entiers. */
    private fun pctAt(t: Double): Double {
        val k = t.toInt().coerceIn(0, 179)
        val f = (t - k).coerceIn(0.0, 1.0)
        return profile[k] * (1 - f) + profile[k + 1] * f
    }

    private fun drawVmgCircle(c: Canvas) {
        // Angles optimum de chaque allure, lus dans le profil.
        var kUp = 0; var kDn = 180
        for (k in 0..89) if (profile[k] > profile[kUp]) kUp = k
        for (k in 90..180) if (profile[k] > profile[kDn]) kDn = k
        val step = 360.0 / STEPS
        for (i in 0 until STEPS) {
            // Angle relatif à l'étrave → TWA (bâbord et tribord symétriques)
            val t = abs(((i + 0.5) * step + 540) % 360 - 180)
            val redSide = if (t < 90) t < kUp else t > kDn   // pincé / trop abattu
            base[i] = vmgColor(pctAt(t), redSide)
        }
        // Lissage purement graphique : moyenne glissante des couleurs le long de l'arc.
        val n = 2 * SMOOTH + 1
        for (i in 0 until STEPS) {
            var r = 0; var g = 0; var b = 0
            for (d in -SMOOTH..SMOOTH) {
                val col = base[(i + d + STEPS) % STEPS]
                r += (col shr 16) and 0xFF; g += (col shr 8) and 0xFF; b += col and 0xFF
            }
            smooth[i] = (0xFF shl 24) or ((r / n) shl 16) or ((g / n) shl 8) or (b / n)
        }
        circleOk = true
        stroke.strokeWidth = R_GAP_OUT - R_GAP_IN; stroke.pathEffect = null
        val rr = (R_GAP_IN + R_GAP_OUT) / 2
        oval.set(CX - rr, CY - rr, CX + rr, CY + rr)
        val sweep = (step * 1.3).toFloat()          // léger recouvrement, pas de fente
        for (i in 0 until STEPS) {
            stroke.color = smooth[i]
            c.drawArc(oval, (i * step - 90).toFloat(), sweep, false, stroke)
        }
    }

    /** Trait fin en travers du cercle des allures à l'angle optimum de l'allure en cours
     *  (TARGET_TWA, celui des laylines), des deux bords : cœur vert (#008000), bords d'un vert
     *  intermédiaire avec le vert de la plage 99 %. Repère relatif à l'étrave. */
    private fun drawOptimumTicks(c: Canvas) {
        val t = v(iTgt) ?: return
        stroke.pathEffect = null
        for ((w, col) in arrayOf(3f to 0xFF00BF00.toInt(), 1f to 0xFF008000.toInt())) {
            stroke.color = col; stroke.strokeWidth = w
            for (a in doubleArrayOf(t, -t))
                c.drawLine(px(a, R_GAP_IN), py(a, R_GAP_IN), px(a, R_GAP_OUT), py(a, R_GAP_OUT), stroke)
        }
    }

    /** Couleur du cercle à un angle relatif à l'étrave (rad). */
    private fun circleColorAt(a: Double): Int {
        val i = ((a * DEG / (360.0 / STEPS)).roundToInt() % STEPS + STEPS) % STEPS
        return smooth[i]
    }

    /** Vert ≥ 99 %, dégradé jusqu'à rouge (côté pincé / trop abattu) ou bleu (côté travers) à 90 %. */
    private fun vmgColor(pct: Double, redSide: Boolean): Int {
        val t = ((99.0 - pct) / 9.0).coerceIn(0.0, 1.0)
        return lerpColor(0xFF00FF00.toInt(), if (redSide) 0xFFFF0000.toInt() else 0xFF0000FF.toInt(), t)
    }

    /** Secteur depuis le centre entre deux angles écran (ordre quelconque, < 180°). */
    private fun wedge(c: Canvas, a0: Double, a1: Double, color: Int, alpha: Int, r: Float) {
        var start = a0
        var sweep = ((a1 - a0) * DEG)
        sweep = ((sweep % 360) + 360) % 360
        if (sweep > 180) { start = a1; sweep = 360 - sweep }
        fill.color = color; fill.alpha = alpha
        oval.set(CX - r, CY - r, CX + r, CY + r)
        c.drawArc(oval, arcStart(start), sweep.toFloat(), true, fill)
        fill.alpha = 255
    }

    // Opacités (0–255) de l'historique des laylines et de chaque couche de la bande floue,
    // renforcées pour rester lisibles sur le sail chart du centre.
    private val LAY_HIST_ALPHA = 0xA0
    private val LAY_BAND_ALPHA = 0x2C

    /** [toCard] : écart à ajouter à une direction vraie pour la lire sur la carte (variation en mode M). */
    private fun drawLaylines(c: Canvas, toCard: Double) {
        val r = R_CARD_IN
        val twa = v(iTwa)
        val onStbd = twa != null && twa > 0
        // Historique (secteurs translucides)
        val lsMin = v(iLsMin); val lsMax = v(iLsMax); val lpMin = v(iLpMin); val lpMax = v(iLpMax)
        if (lsMin != null && lsMax != null) wedge(c, sa(lsMin), sa(lsMax), p.stbd, LAY_HIST_ALPHA, r)
        if (lpMin != null && lpMax != null) wedge(c, sa(lpMin), sa(lpMax), p.port, LAY_HIST_ALPHA, r)

        // Bande d'incertitude de l'amure opposée : dégradé vers les bords.
        val lo = v(if (onStbd) iLpLo else iLsLo)
        val hi = v(if (onStbd) iLpHi else iLsHi)
        val mid = v(if (onStbd) iLp else iLs)
        val color = if (onStbd) p.port else p.stbd
        if (twa != null && lo != null && hi != null && mid != null) {
            for (k in 1..8) {
                val f = k / 8.0
                wedge(c, sa(mid + f * angDiff(lo, mid)), sa(mid + f * angDiff(hi, mid)), color, LAY_BAND_ALPHA, r)
            }
        }

        // Laylines (trait plein)
        stroke.strokeWidth = 5f; stroke.pathEffect = null
        v(iLs)?.let { stroke.color = p.stbd; c.drawLine(CX, CY, px(sa(it), r), py(sa(it), r), stroke) }
        v(iLp)?.let { stroke.color = p.port; c.drawLine(CX, CY, px(sa(it), r), py(sa(it), r), stroke) }

        // Secteur entre l'amure opposée au même TWA et la layline (angle optimum),
        // sur le bord intérieur de la carte.
        if (twa != null && mid != null) {
            v(iOppTwa)?.let { o ->
                val d = angDiff(o, mid)
                if (abs(d) > 0.5 / DEG) {
                    oval.set(CX - R_TAG, CY - R_TAG, CX + R_TAG, CY + R_TAG)
                    stroke.color = color; stroke.alpha = OPP_SECTOR_ALPHA
                    stroke.strokeWidth = OPP_SECTOR_W; stroke.pathEffect = null
                    c.drawArc(oval, arcStart(sa(mid)), (d * DEG).toFloat(), false, stroke)
                    stroke.alpha = 255
                }
            }
        }
    }

    private val OPP_SECTOR_ALPHA = 0x90
    private val OPP_SECTOR_W = 26f

    /**
     * Traînée du vent (10 min) : angle = valeur passée, distance
     * au centre = âge, du bord intérieur de la carte (maintenant) vers le centre.
     * Le trait s'efface avec l'âge ; coupé si une donnée manque ou saute de plus de 90°.
     */
    private fun drawWindTrails(c: Canvas, boatRot: Double) {
        val hist = Engine.windHistory()
        if (hist.size < 2) return
        val now = SystemClock.elapsedRealtime()
        // Historique du TWD (direction vraie, lue sur la carte) à la place de TWA/AWA.
        // Couleur : écart au TWD moyen 10 min, vu de l'amure actuelle. Vent plus à l'arrière
        // (adonnante) → vert, plus sur l'avant (refus) → rouge, franc à ±TWD_SHIFT_FULL.
        val avg = v(iTwdAvg); val twa = v(iTwa)
        val side = if (twa == null) 0.0 else sign(twa)       // TWA > 0 : TWD qui augmente = adonne
        trail(c, hist, now, 0.0, { it.twd?.let { d -> sa(d) } }) { smp ->
            val d = smp.twd
            if (avg == null || d == null || side == 0.0) p.fg
            else {
                val t = (angDiff(d, avg) * side * DEG / TWD_SHIFT_FULL).coerceIn(-1.0, 1.0)
                if (t >= 0) lerpColor(0xFFFFFFFF.toInt(), 0xFF00FF00.toInt(), t)
                else lerpColor(0xFFFFFFFF.toInt(), 0xFFFF2020.toInt(), -t)
            }
        }
    }

    /**
     * Segments dessinés dans un calque en mode SRC : un bout arrondi qui chevauche le
     * segment voisin remplace ses pixels au lieu de doubler leur opacité (sinon points
     * plus clairs aux jonctions des segments longs, quand le vent tourne vite). Chaque
     * segment est en dégradé d'une extrémité à l'autre : pas de marches de couleur.
     */
    private fun trail(c: Canvas, hist: List<Engine.WindSample>, now: Long, boatRot: Double,
                      get: (Engine.WindSample) -> Double?, color: (Engine.WindSample) -> Int) {
        fun withAlpha(col: Int, f: Float) = (col and 0xFFFFFF) or ((255 * (1f - 0.8f * f)).toInt() shl 24)
        val layer = c.saveLayer(null, null)
        stroke.strokeWidth = TRAIL_W; stroke.pathEffect = null
        stroke.strokeCap = Paint.Cap.ROUND
        stroke.xfermode = PorterDuffXfermode(PorterDuff.Mode.SRC)
        for (i in 1 until hist.size) {
            val a0 = get(hist[i - 1]) ?: continue
            val a1 = get(hist[i]) ?: continue
            if (abs(angDiff(a1, a0)) > PI / 2) continue
            val f0 = ((now - hist[i - 1].t).toFloat() / Engine.WIND_HISTORY_MS).coerceIn(0f, 1f)
            val f1 = ((now - hist[i].t).toFloat() / Engine.WIND_HISTORY_MS).coerceIn(0f, 1f)
            val r0 = R_TRAIL_OUT - f0 * (R_TRAIL_OUT - R_TRAIL_IN)
            val r1 = R_TRAIL_OUT - f1 * (R_TRAIL_OUT - R_TRAIL_IN)
            val x0 = px(a0 + boatRot, r0); val y0 = py(a0 + boatRot, r0)
            val x1 = px(a1 + boatRot, r1); val y1 = py(a1 + boatRot, r1)
            stroke.shader = LinearGradient(x0, y0, x1, y1, withAlpha(color(hist[i - 1]), f0),
                                           withAlpha(color(hist[i]), f1), Shader.TileMode.CLAMP)
            c.drawLine(x0, y0, x1, y1, stroke)
        }
        stroke.shader = null; stroke.xfermode = null
        stroke.alpha = 255; stroke.strokeCap = Paint.Cap.BUTT
        c.restoreToCount(layer)
    }

    private val R_TRAIL_OUT = (540f - NEEDLE_BASE) * K   // base des triangles TWA/AWA
    private val R_TRAIL_IN = R_CARD_IN * 0.2f        // rayon de l'échantillon le plus ancien (80 % de profondeur)
    private val TRAIL_W = 4f
    private val TWD_SHIFT_FULL = 10.0                 // ° d'écart pour la couleur franche

    /** Milieu de l'anneau libre entre le cercle des allures et le bord de la rose. */
    private val R_TAG = R_CARD_IN - OPP_SECTOR_W / 2  // secteur du bord opposé : bord intérieur de la carte

    /** Écart angulaire a − b ramené dans (−π, π]. */
    private fun angDiff(a: Double, b: Double): Double {
        var d = (a - b) % (2 * PI)
        if (d > PI) d -= 2 * PI
        if (d <= -PI) d += 2 * PI
        return d
    }

    /**
     * Jauge d'angle de barre dans le bas du bateau : arc concentrique au cadran.
     * Échelle ±15° de barre étalée sur ±28° d'arc (entre les coques), graduations tous
     * les 5° de barre. Hypothèse à vérifier à bord : barre > 0 = tribord → droite.
     */
    private fun drawRudder(c: Canvas) {
        val r = 205f                                   // rayon de l'arc
        val arc = 28.0                                 // ± degrés d'arc dessinés (entre les coques)
        val range = 15.0                               // ± degrés de barre affichés
        val k = arc / range
        stroke.pathEffect = null; stroke.strokeWidth = 3f; stroke.color = p.label
        oval.set(CX - r, CY - r, CX + r, CY + r)
        c.drawArc(oval, (90 - arc).toFloat(), (2 * arc).toFloat(), false, stroke)
        for (d in -15..15 step 5) {
            val a = PI - d * k / DEG                   // bas de la rose, tribord à droite
            val len = if (d % 15 == 0) 18f else 11f
            stroke.strokeWidth = if (d == 0) 4f else 2f
            c.drawLine(px(a, r), py(a, r), px(a, r - len), py(a, r - len), stroke)
        }
        val rud = v(iRudder)
        // Historique 2 min (tenu par le moteur, survit à l'écran) : barre verte du min au max.
        Engine.rudderRange()?.let { (lo0, hi0) ->
            val lo = lo0.coerceIn(-range, range)
            val hi = hi0.coerceIn(-range, range)
            val rh = r + 9f
            stroke.color = 0xFF2ECC40.toInt(); stroke.strokeWidth = 8f
            oval.set(CX - rh, CY - rh, CX + rh, CY + rh)
            // Angle écran (horaire depuis 3 h) : bas = 90°, tribord vers la droite = plus petit.
            val a0 = (90 - hi * k).toFloat()
            val sweep = ((hi - lo) * k).toFloat().coerceAtLeast(1.5f)
            c.drawArc(oval, a0, sweep, false, stroke)
        }
        if (rud == null) return
        val deg = (rud * DEG).coerceIn(-range, range)
        val a = PI - deg * k / DEG
        val col = if (rud >= 0) p.stbd else p.port
        // Repère : triangle pointant vers l'arc, à l'intérieur.
        path.reset()
        path.moveTo(px(a, r - 2), py(a, r - 2))
        path.lineTo(px(a + 0.07, r - 30), py(a + 0.07, r - 30))
        path.lineTo(px(a - 0.07, r - 30), py(a - 0.07, r - 30))
        path.close()
        fill.color = col
        c.drawPath(path, fill)
        text.textSize = 30f; text.textAlign = Paint.Align.CENTER; text.color = p.fg
        c.drawText(abs(rud * DEG).roundToInt().toString(), CX, CY + r - 42f, text)
    }

    private val CURRENT_COL = 0xB300B3B3.toInt()          // bleu-vert, opacité 70 %

    /** Flèche de courant centrée sur la rose, longueur selon la vitesse. */
    private fun drawCurrent(c: Canvas) {
        val set = v(iSet) ?: return
        val drift = v(iDrift) ?: return
        val kn = drift * KN
        if (kn < 0.1) return
        // Longueur proportionnelle à la vitesse, plafonnée à 4 kn.
        val len = ((min(kn, 4.0) / 3.0 * 200 + 70) * 1.3).toFloat()   // agrandie de 30 %
        val w = len * 0.56f                              // flèche en pointe de dard, encoche à l'arrière
        val a = sa(set)
        c.save()
        c.rotate((a * DEG).toFloat(), CX, CY)
        val top = CY - len / 2; val bot = CY + len / 2
        path.reset()
        path.moveTo(CX, top); path.lineTo(CX + w / 2, bot)
        path.lineTo(CX, bot - len * 0.27f); path.lineTo(CX - w / 2, bot); path.close()
        fill.color = CURRENT_COL; c.drawPath(path, fill)          // bleu-vert, 70 % : on voit dessous
        c.restore()
        // Force du courant au centre de la rose, blanc cerné de noir.
        val str = fmt1(kn)
        text.textSize = 39f; text.textAlign = Paint.Align.CENTER        // 56 − 30 %
        text.style = Paint.Style.STROKE; text.strokeWidth = 6f; text.color = 0xFF000000.toInt()
        c.drawText(str, CX, CY + 14f, text)
        text.style = Paint.Style.FILL; text.color = p.fg
        c.drawText(str, CX, CY + 14f, text)
    }

    // Indicateurs de connexion : coin bas gauche (qtVlm au-dessus d'o3nav).
    private fun drawStatus(c: Canvas) {
        val now = SystemClock.elapsedRealtime()
        val l = link
        val fresh = l != null && l.state == YdrawLink.State.CONNECTED && now - l.lastLineMs < 3000
        text.textSize = 24f; text.color = p.label
        fill.color = if (fresh) p.ok else p.bad
        c.drawCircle(262f, VH - 22, 9f, fill)
        text.textAlign = Paint.Align.LEFT
        val msg = when {
            fresh -> "N2K"
            l != null && l.state == YdrawLink.State.CONNECTED -> "N2K : muet"
            else -> "N2K : coupé"
        }
        c.drawText(msg, 280f, VH - 13, text)
        // Navigation qtVlm, au-dessus d'o3nav : vert si reçue depuis moins de 10 s,
        // gris sinon (pas une panne : qtVlm n'émet que s'il navigue vers un WP).
        val last = navLast()
        val navFresh = last != 0L && now - last < 10_000
        fill.color = if (navFresh) p.ok else p.stale
        c.drawCircle(262f, VH - 57, 9f, fill)
        text.textAlign = Paint.Align.LEFT; text.textSize = 24f; text.color = p.label
        c.drawText("qtVlm", 280f, VH - 48, text)
        // Nom du WP de destination, seul en bas à droite.
        core.wpName(now)?.let { name ->
            text.textSize = 30f; text.color = p.fg; text.textAlign = Paint.Align.RIGHT
            val maxW = 190f
            val w = text.measureText(name)
            if (w > maxW) text.textSize = 30f * maxW / w
            c.drawText(name, VW - 252f, VH - 13, text)
        }
    }

    /** Case sous le doigt : (colonne 0/1/2, index 0..4), ou null. */
    fun cellAt(x: Float, y: Float): Pair<Int, Int>? {
        val s = min(width / VW, height / VH)
        val vx = (x - (width - VW * s) / 2f) / s
        val vy = (y - (height - VH * s) / 2f) / s
        if (vy < 0 || vy >= VH) return null
        val idx = (vy / 160f).toInt().coerceIn(0, 4)
        return when {
            vx in 0f..240f -> 0 to idx
            vx in VW - 240f..VW -> (if (wpActive()) 2 else 1) to idx
            else -> null
        }
    }

    /** Points rouges des alertes AIS en cours (MMSI, x, y en repère virtuel, rose réduite comprise). */
    private val aisDots = ArrayList<Triple<Long, Float, Float>>()
    private val R_AIS_DOT = (R_GAP_IN + R_GAP_OUT) / 2

    /** Point rouge sur le cercle des allures dans la direction de chaque cible en alerte
     *  (balise de détresse, cible dangereuse) ; il pulse tant que l'alerte n'est pas acquittée. */
    private fun drawAisDots(c: Canvas) {
        aisDots.clear()
        val now = SystemClock.elapsedRealtime()
        for (t in (if (aisMob) aisMobs else listOfNotNull(aisDistress, aisThreat?.takeIf { it.level == 2 }))) {
            if (noDir(t) || aisDots.any { it.first == t.mmsi }) continue
            val a = sa(t.bearing)
            val x = px(a, R_AIS_DOT); val y = py(a, R_AIS_DOT)
            val acked = t.mmsi in Engine.aisAcked
            val r = if (acked) 14f else 14f + 6f * ((1 - cos(2 * PI * (now % 1000) / 1000.0)) / 2).toFloat()
            fill.color = 0xFFFF2020.toInt(); c.drawCircle(x, y, r, fill)
            stroke.color = 0xFFFFFFFF.toInt(); stroke.strokeWidth = 3f; stroke.pathEffect = null
            c.drawCircle(x, y, r, stroke)
            aisDots += Triple(t.mmsi, CX + (x - CX) * ROSE_S, CY + (y - CY) * ROSE_S)
        }
    }

    /** Direction inconnue : pas de position, ou position estimée (celle du bateau à la
     *  réception de l'alerte) dont on ne s'est pas encore assez écarté. */
    private fun noDir(t: N2kCore.AisTarget) = t.bearing.isNaN() || t.est && t.range < 20.0

    /** MMSI de l'alerte AIS sous le doigt (point rouge, ou bandeau de détresse si sa
     *  direction est inconnue), ou null. */
    fun aisAlertAt(x: Float, y: Float): Long? {
        val s = min(width / VW, height / VH)
        val vx = (x - (width - VW * s) / 2f) / s
        val vy = (y - (height - VH * s) / 2f) / s
        aisDots.minByOrNull { hypot(it.second - vx, it.third - vy) }
            ?.takeIf { hypot(it.second - vx, it.third - vy) < 50f }?.let { return it.first }
        val d = aisDistress
        if (d != null && noDir(d) && vx in BAN_L..BAN_R && vy in BAN_T..BAN_B) return d.mmsi
        return null
    }

    /** Coordonnées écran sur le bouton du sail chart (coin haut gauche de la rose). */
    fun isSailButton(x: Float, y: Float): Boolean {
        val s = min(width / VW, height / VH)
        val vx = (x - (width - VW * s) / 2f) / s
        val vy = (y - (height - VH * s) / 2f) / s
        return vy in 0f..90f && vx in 240f..350f
    }

    /** Coordonnées écran dans une zone d'indicateurs (ouvre les réglages). */
    fun isStatusArea(x: Float, y: Float): Boolean {
        val s = min(width / VW, height / VH)
        val vx = (x - (width - VW * s) / 2f) / s
        val vy = (y - (height - VH * s) / 2f) / s
        return vy > VH - 100 && vx in 240f..440f          // indicateurs, coin bas gauche
    }
}
