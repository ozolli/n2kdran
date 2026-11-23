package oz.n2kdran

import android.content.Context
import java.io.File

/** Réglages persistants (SharedPreferences) et polaire choisie (copiée dans l'app). */
class Settings(context: Context) {
    private val app = context.applicationContext
    private val sp = app.getSharedPreferences("n2kdran", Context.MODE_PRIVATE)

    var host: String
        get() = sp.getString("host", DEFAULT_HOST) ?: DEFAULT_HOST
        set(v) = sp.edit().putString("host", v).apply()

    var port: Int
        get() = sp.getInt("port", DEFAULT_PORT)
        set(v) = sp.edit().putInt("port", v).apply()

    /** Dernières sources N2K auxquelles la connexion a réussi, la plus récente d'abord. */
    val recentSources: List<Pair<String, Int>>
        get() = (sp.getString("recentSources", "") ?: "").split('|').mapNotNull { e ->
            val i = e.lastIndexOf(':')
            val p = if (i > 0) e.substring(i + 1).toIntOrNull() else null
            if (p == null) null else e.substring(0, i) to p
        }

    fun addRecentSource(host: String, port: Int) {
        val l = (listOf(host to port) + recentSources.filter { it != host to port }).take(RECENT_MAX)
        sp.edit().putString("recentSources", l.joinToString("|") { "${it.first}:${it.second}" }).apply()
    }

    /** Relais de la navigation vers 127.0.0.1:navRelayPort (qtVlm local), 0 = aucun. */
    var navRelayPort: Int
        get() = sp.getInt("navRelayPort", 0)
        set(v) = sp.edit().putInt("navRelayPort", v).apply()

    var navPort: Int
        get() = sp.getInt("navPort", DEFAULT_NAV_PORT)
        set(v) = sp.edit().putInt("navPort", v).apply()

    var staleS: Int
        get() = sp.getInt("staleS", DEFAULT_STALE_S)
        set(v) = sp.edit().putInt("staleS", v).apply()

    /** Référence du cap affiché : 'V' vrai, 'M' magnétique, 'C' COG. */
    var headingRef: Char
        get() = (sp.getString("headingRef", "V") ?: "V").firstOrNull()?.takeIf { it in "VMC" } ?: 'V'
        set(v) = sp.edit().putString("headingRef", v.toString()).apply()

    /** Sail chart en polaire au centre de la rose (le nom de la voile reste toujours affiché). */
    var aisIgnoreStill: Boolean
        get() = sp.getBoolean("aisIgnoreStill", false)
        set(v) = sp.edit().putBoolean("aisIgnoreStill", v).apply()

    var sailRose: Boolean
        get() = sp.getBoolean("sailRose", true)
        set(v) = sp.edit().putBoolean("sailRose", v).apply()

    /** Atténuation de nuit : opacité du voile noir, en % (0–90). */
    /** Immersion du transducteur sous la flottaison (m), si le sondeur n'envoie pas de décalage positif. */
    var transducerDepth: Double
        get() = sp.getFloat("transducerDepth", 0f).toDouble()
        set(v) = sp.edit().putFloat("transducerDepth", v.toFloat()).apply()

    var nightDimPct: Int
        get() = sp.getInt("nightDim", DEFAULT_NIGHT_DIM)
        set(v) = sp.edit().putInt("nightDim", v).apply()

    /** Cible AIS dangereuse : CPA (NM) et TCPA (min) sous ces seuils. */
    var aisCpaNm: Double
        get() = sp.getFloat("aisCpa", DEFAULT_AIS_CPA.toFloat()).toDouble()
        set(v) = sp.edit().putFloat("aisCpa", v.toFloat()).apply()

    var aisRangeNm: Double
        get() = sp.getFloat("aisRange", DEFAULT_AIS_RANGE.toFloat()).toDouble()
        set(v) = sp.edit().putFloat("aisRange", v.toFloat()).apply()

    var aisTcpaMin: Int
        get() = sp.getInt("aisTcpa", DEFAULT_AIS_TCPA)
        set(v) = sp.edit().putInt("aisTcpa", v).apply()

    /** Nom affiché de la polaire choisie (null = CM50 embarquée). */
    var polarName: String?
        get() = sp.getString("polarName", null)
        set(v) = sp.edit().putString("polarName", v).apply()

    val polarFile: File get() = File(app.filesDir, "polaire.pol")

    /** Contenu des cases : colonne 0 gauche, 1 droite sans WP, 2 droite avec WP. */
    fun column(col: Int): List<String> {
        val v = sp.getString("cells$col", null)?.split(',')
        return if (v != null && v.size == 5) v else Cells.DEFAULTS[col]
    }

    fun setCell(col: Int, idx: Int, key: String) {
        val l = column(col).toMutableList()
        l[idx] = key
        sp.edit().putString("cells$col", l.joinToString(",")).apply()
    }

    /** Sail chart choisi (null = CM50 embarqué) et ses deux fichiers copiés dans l'app. */
    var sailName: String?
        get() = sp.getString("sailName", null)
        set(v) = sp.edit().putString("sailName", v).apply()
    val saildefFile: File get() = File(app.filesDir, "voiles.saildef")
    val sailselectFile: File get() = File(app.filesDir, "voiles.sailselect")

    /** Contenu du sail chart à utiliser : (nom, saildef, sailselect). */
    fun sailTexts(): Triple<String, String, String> {
        if (sailName != null && saildefFile.isFile && sailselectFile.isFile)
            return Triple(sailName ?: "sail chart", saildefFile.readText(), sailselectFile.readText())
        fun asset(n: String) = app.assets.open("sailchart/$n").use { it.readBytes().toString(Charsets.UTF_8) }
        return Triple("CM50 (défaut)", asset("CM50.saildef"), asset("CM50.sailselect"))
    }

    /** Contenu de la polaire à utiliser : fichier choisi, sinon CM50 embarquée. */
    fun polarBytes(): Pair<String, ByteArray> {
        val f = polarFile
        if (polarName != null && f.isFile) return (polarName ?: "polaire") to f.readBytes()
        return "CM50 (défaut)" to app.assets.open("polars/CM50.pol").use { it.readBytes() }
    }

    companion object {
        const val DEFAULT_HOST = "10.3.0.2"
        const val DEFAULT_PORT = 2700
        const val DEFAULT_NAV_PORT = 2800
        const val DEFAULT_STALE_S = 3
        const val DEFAULT_NIGHT_DIM = 60
        const val RECENT_MAX = 3
        const val DEFAULT_AIS_CPA = 0.5
        const val DEFAULT_AIS_TCPA = 20
        const val DEFAULT_AIS_RANGE = 15.0
    }
}
