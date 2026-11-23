package oz.n2kdran

import android.content.Context
import android.os.SystemClock
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch

/**
 * Moteur de n2kdran : cœur C, connexions et historiques, unique pour tout le
 * processus. Il vit dans le service de premier plan (N2kService) et continue de
 * recevoir et de calculer quand l'écran n'est pas affiché ; l'activité ne fait
 * que lire l'état et l'afficher.
 */
object Engine {
    val core: N2kCore by lazy { N2kCore() }

    @Volatile var link: YdrawLink? = null; private set
    @Volatile var navUdp: NavUdpLink? = null; private set
    @Volatile var host = Settings.DEFAULT_HOST; private set
    @Volatile var port = Settings.DEFAULT_PORT; private set
    @Volatile var polarMsg = ""; private set

    /** Sail chart chargé (null si invalide) et message pour la mise au point. */
    @Volatile var sailChart: SailChart? = null; private set
    @Volatile var sailMsg = ""; private set
    /** Configuration recommandée, retenue après 3 s de stabilité (null = inconnue). */
    @Volatile var sailCode: Int? = null; private set

    /** Adresse imposée au lancement (mise au point : --es host …), sinon réglages. */
    @Volatile var hostOverride: String? = null

    /** Appelé quand l'utilisateur arrête n2kdran depuis la notification. */
    @Volatile var onStopped: (() -> Unit)? = null

    private var app: Context? = null
    private var scope: CoroutineScope? = null

    val running: Boolean get() = scope != null

    @Synchronized
    fun start(context: Context) {
        if (scope != null) return
        val ctx = context.applicationContext
        app = ctx
        LanNetwork.start(ctx)
        scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
        applySettings()
        startRudderSampler()
    }

    /** (Re)crée les connexions et applique polaire et péremption selon les réglages. */
    @Synchronized
    fun applySettings() {
        val ctx = app ?: return
        val sc = scope ?: return
        val s = Settings(ctx)
        stopLinks()
        host = hostOverride ?: s.host
        port = s.port
        core.setStaleMs(s.staleS * 1000)
        core.setAisIgnoreStill(s.aisIgnoreStill)
        aisCpaM = s.aisCpaNm * 1852.0
        aisTcpaS = s.aisTcpaMin * 60.0
        aisRangeM = s.aisRangeNm * 1852.0
        polarMsg = try {
            val (name, bytes) = s.polarBytes()
            core.setPolar(bytes)?.let { "polaire $name : $it" } ?: "polaire $name"
        } catch (e: Exception) {
            "polaire : ${e.message}"
        }
        sailMsg = try {
            val (name, def, sel) = s.sailTexts()
            sailChart = SailChart.parse(def, sel)
            "voiles $name"
        } catch (e: Exception) {
            sailChart = null
            "voiles : ${e.message}"
        }
        sailCode = null; sailCandidate = null
        val h = host; val p = port
        link = YdrawLink(core, h, p, onConnected = { s.addRecentSource(h, p) }).also { it.start(sc) }
        navUdp = NavUdpLink(ctx, core, s.navPort, s.navRelayPort).also { it.start(sc) }
    }

    /** Arrêt complet (bouton « Arrêter » de la notification). */
    @Synchronized
    fun stop() {
        stopLinks()
        core.aisReset()                              // fin des alertes MOB mémorisées
        aisAcked.clear(); aisSkipped.clear()
        scope?.cancel()
        scope = null
        onStopped?.invoke()
    }

    private fun stopLinks() {
        link?.stop(); link = null
        navUdp?.stop(); navUdp = null
    }

    /** Dernière réception de navigation qtVlm (horloge elapsedRealtime), 0 = jamais. */
    fun navLastMs(): Long = navUdp?.lastPacketMs ?: 0L

    // --- Échantillonnage (5 Hz) : historique de barre (2 min) et voilure ---
    private val rudHist = ArrayDeque<Pair<Long, Double>>()      // (temps, angle en °)
    private const val RUDDER_HISTORY_MS = 120_000L

    /** Historique du vent pour les traînées de la rose : (temps, TWA, AWA, TWD) en rad, null si absent. */
    class WindSample(val t: Long, val twa: Double?, val awa: Double?, val twd: Double?)
    private val windHist = ArrayDeque<WindSample>()
    const val WIND_HISTORY_MS = 600_000L
    private const val WIND_SAMPLE_MS = 1_000L
    private var windLastT = 0L

    // Configuration de voilure : la case candidate doit tenir SAIL_STABLE_MS.
    private var sailCandidate: Int? = null
    private var sailCandidateT = 0L
    private const val SAIL_STABLE_MS = 3_000L

    private fun updateSail(now: Long, twaRad: Double?, twsMs: Double?) {
        val chart = sailChart
        if (chart == null || twaRad == null || twsMs == null) { sailCode = null; sailCandidate = null; return }
        val code = chart.codeAt(Math.toDegrees(twaRad), twsMs * 3600.0 / 1852.0)
        if (sailCode == null) { sailCode = code; sailCandidate = code; return }   // première valeur
        if (code != sailCandidate) { sailCandidate = code; sailCandidateT = now }
        if (code != sailCode && now - sailCandidateT >= SAIL_STABLE_MS) sailCode = code
    }

    private fun startRudderSampler() {
        val iRudder = core.idx("RUDDER")
        val iTwa = core.idx("TWA")
        val iTws = core.idx("TWS")
        val iAwa = core.idx("AWA")
        val iTwd = core.idx("TWD")
        val v = DoubleArray(core.names.size)
        val st = ByteArray(core.names.size)
        scope?.launch {
            while (isActive) {
                val now = SystemClock.elapsedRealtime()
                core.snapshotInto(now, v, st)
                synchronized(rudHist) {
                    if (iRudder >= 0 && st[iRudder] == N2kCore.OK)
                        rudHist.addLast(now to Math.toDegrees(v[iRudder]))
                    while (rudHist.isNotEmpty() && now - rudHist.first().first > RUDDER_HISTORY_MS)
                        rudHist.removeFirst()
                }
                fun ok(i: Int) = if (i >= 0 && st[i] == N2kCore.OK) v[i] else null
                if (now - windLastT >= WIND_SAMPLE_MS) synchronized(windHist) {
                    windLastT = now
                    windHist.addLast(WindSample(now, ok(iTwa), ok(iAwa), ok(iTwd)))
                    while (windHist.isNotEmpty() && now - windHist.first().t > WIND_HISTORY_MS)
                        windHist.removeFirst()
                }
                updateSail(now, ok(iTwa), ok(iTws))
                // Cibles passées sautées ; cibles acquittées muettes.
                val skip = aisSkipped.toList()
                // Homme à la mer : toutes les autres alertes AIS sont coupées ; la
                // sonnerie continue tant qu'un MOB n'est pas acquitté.
                val mobs = core.aisMobs(now)
                if (mobs.isNotEmpty()) {
                    distressAlarm(now, mobs.firstOrNull { it.mmsi !in aisAcked })
                    dangerBeep(now, null)
                } else {
                    val distress = core.aisDistress(now, aisAcked.toList())
                    val threat = core.aisThreat(now, aisCpaM, aisTcpaS, aisRangeM, skip)?.takeIf { it.level == 2 }
                    if (aisAcked.isNotEmpty() && core.aisDistress(now) == null &&
                        core.aisThreat(now, aisCpaM, aisTcpaS, aisRangeM)?.level != 2) {
                        aisAcked.clear(); aisSkipped.clear()   // plus aucune alerte : réarmement
                    }
                    distressAlarm(now, distress)
                    dangerBeep(now, threat?.takeIf { it.mmsi !in aisAcked })
                }
                delay(200)
            }
        }
    }

    // --- Alarme sonore des balises de détresse AIS (SART, MOB, EPIRB) ---
    // Flux « alarme » : sonne même téléphone en silencieux (sauf Ne pas déranger
    // réglé pour couper les alarmes). Jouée par le service, écran éteint compris.
    private var tone: android.media.ToneGenerator? = null
    private var alarmLastT = 0L
    private const val ALARM_PERIOD_MS = 1_500L

    /** MMSI des alertes AIS acquittées (1er appui sur le point rouge de la rose) : plus de
     *  son, de bandeau ni de couronne pulsante, la cible reste affichée. */
    val aisAcked: MutableSet<Long> = java.util.concurrent.ConcurrentHashMap.newKeySet()
    /** MMSI passés (2e appui) : sautés, on affiche la cible suivante. Les deux
     *  ensembles sont vidés quand plus aucune alerte n'est en cours. */
    val aisSkipped: MutableSet<Long> = java.util.concurrent.ConcurrentHashMap.newKeySet()

    /** Appui sur le point rouge d'une cible : acquitte, ou passe à la suivante si déjà acquittée. */
    fun aisTap(mmsi: Long) {
        val mob = core.aisMobs(SystemClock.elapsedRealtime()).any { it.mmsi == mmsi }
        if (!aisAcked.add(mmsi) && !mob) aisSkipped += mmsi   // MOB : acquittement du son seulement
    }

    private fun distressAlarm(now: Long, d: N2kCore.AisTarget?) {
        if (d == null) {
            if (tone != null) {
                tone?.release(); tone = null
                restoreAlarmVolume()
            }
            return
        }
        if (now - alarmLastT < ALARM_PERIOD_MS) return
        alarmLastT = now
        val tg = tone ?: try {
            maxAlarmVolume()
            android.media.ToneGenerator(android.media.AudioManager.STREAM_ALARM, 100).also { tone = it }
        } catch (e: RuntimeException) { null } ?: return
        tg.startTone(android.media.ToneGenerator.TONE_CDMA_EMERGENCY_RINGBACK, 1_000)
    }

    // --- Bip de cible AIS dangereuse (CPA et TCPA sous les seuils des réglages) ---
    // Flux alarme, au volume réglé (pas forcé au maximum) ; muet pendant une
    // alerte de détresse, qui a sa propre sonnerie.
    @Volatile private var aisCpaM = Settings.DEFAULT_AIS_CPA * 1852.0
    @Volatile private var aisTcpaS = Settings.DEFAULT_AIS_TCPA * 60.0
    @Volatile private var aisRangeM = Settings.DEFAULT_AIS_RANGE * 1852.0
    private var beepTone: android.media.ToneGenerator? = null
    private var beepLastT = 0L
    private const val BEEP_PERIOD_MS = 3_000L

    private fun dangerBeep(now: Long, t: N2kCore.AisTarget?) {
        if (tone != null || t == null) {
            beepTone?.release(); beepTone = null
            return
        }
        if (now - beepLastT < BEEP_PERIOD_MS) return
        beepLastT = now
        val tg = beepTone ?: try {
            android.media.ToneGenerator(android.media.AudioManager.STREAM_ALARM, 100).also { beepTone = it }
        } catch (e: RuntimeException) { null } ?: return
        tg.startTone(android.media.ToneGenerator.TONE_PROP_BEEP2, 400)
    }

    /** Volume « alarme » de l'appareil avant l'alerte (-1 = non modifié). */
    private var savedAlarmVolume = -1

    private fun audio() = app?.getSystemService(Context.AUDIO_SERVICE) as? android.media.AudioManager

    /** Volume « alarme » au maximum pendant l'alerte (refus du système ignoré). */
    private fun maxAlarmVolume() {
        val am = audio() ?: return
        val s = android.media.AudioManager.STREAM_ALARM
        try {
            if (savedAlarmVolume < 0) savedAlarmVolume = am.getStreamVolume(s)
            am.setStreamVolume(s, am.getStreamMaxVolume(s), 0)
        } catch (e: SecurityException) { }
    }

    private fun restoreAlarmVolume() {
        val v = savedAlarmVolume
        if (v < 0) return
        savedAlarmVolume = -1
        try {
            audio()?.setStreamVolume(android.media.AudioManager.STREAM_ALARM, v, 0)
        } catch (e: SecurityException) { }
    }

    /** Copie de l'historique du vent (du plus ancien au plus récent). */
    fun windHistory(): List<WindSample> = synchronized(windHist) { windHist.toList() }

    /** Angles de barre min et max (°) sur 2 min, ou null s'il n'y a pas assez de mesures. */
    fun rudderRange(): Pair<Double, Double>? = synchronized(rudHist) {
        if (rudHist.size < 2) null
        else rudHist.minOf { it.second } to rudHist.maxOf { it.second }
    }
}
