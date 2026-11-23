package oz.n2kdran

/**
 * Enveloppe Kotlin du cœur C (libn2k). Unités SI, cf. libn2k/include/n2k.h.
 * feed() est appelé par le thread de réception, snapshot() par l'affichage :
 * la couche JNI sérialise les accès. Les tableaux values/status ne doivent être
 * lus que par le thread qui appelle snapshot().
 *
 * close() peut survenir pendant qu'un thread de réception est encore en vie
 * (recréation de l'activité) : un verrou lecture/écriture fait attendre la
 * fermeture, et tout appel après fermeture est ignoré (jamais de pointeur libéré).
 */
class N2kCore : AutoCloseable {
    private var h: Long = nativeCreate().also { check(it != 0L) { "libn2k : création impossible" } }
    private val rw = java.util.concurrent.locks.ReentrantReadWriteLock()

    private inline fun <T> use(closed: T, f: (Long) -> T): T {
        val r = rw.readLock()
        r.lock()
        try {
            return if (h == 0L) closed else f(h)
        } finally {
            r.unlock()
        }
    }

    /** Noms des données, dans l'ordre de l'enum C (lu à l'exécution). */
    val names: Array<String> = nativeItemNames()
    private val index: Map<String, Int> = names.withIndex().associate { it.value to it.index }

    val values = DoubleArray(names.size)
    val status = ByteArray(names.size)        // 0 jamais reçu, 1 périmé, 2 frais

    fun idx(name: String): Int = index[name] ?: -1

    fun feed(line: String, nowMs: Long, navOnly: Boolean = false): Boolean =
        use(false) { nativeFeedLine(it, line, nowMs, if (navOnly) FEED_NAV_ONLY else 0) }

    fun snapshot(nowMs: Long) = use(Unit) { nativeSnapshot(it, nowMs, values, status) }

    /** Lecture de l'état dans des tableaux fournis (autre thread que l'affichage). */
    fun snapshotInto(nowMs: Long, v: DoubleArray, st: ByteArray) = use(Unit) { nativeSnapshot(it, nowMs, v, st) }

    /** Installe une polaire qtVlm ; retourne null si OK, sinon le message d'erreur. */
    fun setPolar(text: ByteArray): String? = use("fermé") { nativeSetPolar(it, text) }

    fun setStaleMs(ms: Int) = use(Unit) { nativeSetStaleMs(it, ms) }

    /** Cible AIS immobile (< 0,5 nœud) jamais dangereuse : pas d'alarme. */
    fun setAisIgnoreStill(on: Boolean) = use(Unit) { nativeSetAisIgnoreStill(it, on) }

    /** Profil de VMG (% de l'optimum de l'allure) pour TWA 0..180° ; false si indisponible. */
    fun vmgProfile(out: DoubleArray): Boolean = use(false) { nativeVmgProfile(it, out) }

    /** Nom du WP de destination (129285) s'il est frais, sinon null. */
    fun wpName(nowMs: Long): String? = use(null) { nativeWpName(it, nowMs)?.toString(Charsets.UTF_8) }

    /** Cible AIS : niveau (menace : 1 suivie, 2 dangereuse ; détresse : 970 SART,
     *  972 MOB, 974 EPIRB), distance (m), relèvement vrai (rad), CPA (m), TCPA (s). */
    class AisTarget(val level: Int, val mmsi: Long, val name: String?, val range: Double,
                    val bearing: Double, val cpa: Double, val tcpa: Double,
                    /** Position inconnue, prise à celle du bateau à la réception de l'alerte. */
                    val est: Boolean = false) {
        /** Homme à la mer : MOB AIS (972…) ou alerte N2K 127233. */
        val isMob get() = level == 972 || level == MOB_N2K
    }

    private fun ais(out: DoubleArray, name: ByteArray?): AisTarget? = name?.let {
        AisTarget(out[0].toInt(), out[1].toLong(), it.toString(Charsets.US_ASCII).ifBlank { null },
            out[2], out[3], out[4], out[5], out[6] != 0.0)
    }

    /** Cible AIS la plus menaçante (seuils CPA en m, TCPA en s, portée en m), MMSI
     *  de `skip` exclus (alertes acquittées), sinon null. */
    fun aisThreat(nowMs: Long, cpaMax: Double, tcpaMax: Double, rangeMax: Double,
                  skip: Collection<Long> = emptyList()): AisTarget? = use(null) {
        val o = DoubleArray(7)
        ais(o, nativeAisThreat(it, nowMs, cpaMax, tcpaMax, rangeMax, skipArray(skip), o))
    }

    /** Balise de détresse AIS active la plus proche, MMSI de `skip` exclus, sinon null. */
    fun aisDistress(nowMs: Long, skip: Collection<Long> = emptyList()): AisTarget? = use(null) {
        val o = DoubleArray(7); ais(o, nativeAisDistress(it, nowMs, skipArray(skip), o))
    }

    /** Hommes à la mer en cours (MOB AIS et 127233, mémorisés jusqu'à aisReset). */
    fun aisMobs(nowMs: Long): List<AisTarget> {
        val out = ArrayList<AisTarget>()
        while (out.size < 16) {                       // le cœur rend les MOB en premier
            val d = aisDistress(nowMs, out.map { it.mmsi }) ?: break
            if (!d.isMob) break
            out += d
        }
        return out
    }

    /** Oublie les cibles AIS et les hommes à la mer mémorisés. */
    fun aisReset() = use(Unit) { nativeAisReset(it) }

    private fun skipArray(skip: Collection<Long>): IntArray? =
        if (skip.isEmpty()) null else skip.map { it.toInt() }.toIntArray()

    /** lignes, rejetées, fast-packets OK, fast-packets abandonnés */
    fun stats(): LongArray = LongArray(4).also { a -> use(Unit) { nativeStats(it, a) } }

    override fun close() {
        val w = rw.writeLock()
        w.lock()
        try {
            if (h != 0L) {
                nativeDestroy(h)
                h = 0L
            }
        } finally {
            w.unlock()
        }
    }

    companion object {
        /** Niveau retourné par aisDistress pour une alerte MOB reçue en PGN 127233. */
        const val MOB_N2K = 127233
        const val NEVER: Byte = 0
        const val STALE: Byte = 1
        const val OK: Byte = 2
        private const val FEED_NAV_ONLY = 1        // N2K_FEED_NAV_ONLY

        init { System.loadLibrary("n2kjni") }

        @JvmStatic private external fun nativeCreate(): Long
        @JvmStatic private external fun nativeDestroy(h: Long)
        @JvmStatic private external fun nativeFeedLine(h: Long, line: String, nowMs: Long, flags: Int): Boolean
        @JvmStatic private external fun nativeSnapshot(h: Long, nowMs: Long, values: DoubleArray, status: ByteArray)
        @JvmStatic private external fun nativeItemNames(): Array<String>
        @JvmStatic private external fun nativeSetPolar(h: Long, text: ByteArray): String?
        @JvmStatic private external fun nativeSetStaleMs(h: Long, ms: Int)
        @JvmStatic private external fun nativeSetAisIgnoreStill(h: Long, on: Boolean)
        @JvmStatic private external fun nativeStats(h: Long, out: LongArray)
        @JvmStatic private external fun nativeVmgProfile(h: Long, out: DoubleArray): Boolean
        @JvmStatic private external fun nativeWpName(h: Long, nowMs: Long): ByteArray?
        @JvmStatic private external fun nativeAisThreat(h: Long, nowMs: Long, cpaMax: Double, tcpaMax: Double, rangeMax: Double, skip: IntArray?, out: DoubleArray): ByteArray?
        @JvmStatic private external fun nativeAisReset(h: Long)
        @JvmStatic private external fun nativeAisDistress(h: Long, nowMs: Long, skip: IntArray?, out: DoubleArray): ByteArray?
    }
}
