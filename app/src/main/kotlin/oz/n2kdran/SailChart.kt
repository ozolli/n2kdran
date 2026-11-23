package oz.n2kdran

import kotlin.math.abs
import kotlin.math.exp
import kotlin.math.roundToInt

/**
 * Sail chart au format qtVlm :
 * - `.saildef` : une configuration par ligne, « code;nom » (ex. « 3;1R J3 »).
 *   Le code est aussi l'indice de la couleur dans la palette de qtVlm.
 * - `.sailselect` : grille comme une polaire, 1re ligne « TWA/TWS;0;5;6;… » (TWS en
 *   nœuds), puis « TWA;code;code;… » (TWA en degrés). « N/A » = pas de configuration.
 * Lecture lissée de la grille (comme l'affichage de qtVlm, méthode exacte non
 * connue) : chaque point de grille vote pour son code avec un poids gaussien
 * (σ = 1 maille) selon sa distance en mailles ; le code au plus fort poids l'emporte.
 * Les frontières deviennent des courbes au lieu de marches d'escalier.
 */
class SailChart private constructor(
    val names: Map<Int, String>,
    val twa: DoubleArray,
    val tws: DoubleArray,
    private val grid: Array<IntArray>,          // [ligne TWA][colonne TWS] → code
) {
    /** Code de la configuration pour |TWA| (°) et TWS (kn), ou null hors grille. */
    fun codeAt(twaDeg: Double, twsKn: Double): Int? {
        if (twa.isEmpty() || tws.isEmpty()) return null
        val fi = frac(twa, abs(twaDeg).coerceIn(0.0, 180.0))
        val fj = frac(tws, twsKn.coerceAtLeast(0.0))
        val i0 = fi.roundToInt(); val j0 = fj.roundToInt()
        val votes = HashMap<Int, Double>()
        for (i in (i0 - R)..(i0 + R)) {
            if (i !in twa.indices) continue
            for (j in (j0 - R)..(j0 + R)) {
                if (j !in tws.indices) continue
                val di = i - fi; val dj = j - fj
                val w = exp(-(di * di + dj * dj) / (2 * SIGMA * SIGMA))
                votes.merge(grid[i][j], w, Double::plus)
            }
        }
        return votes.maxByOrNull { it.value }?.key ?: grid[i0][j0]
    }

    /** Position fractionnaire de x sur un axe croissant (en mailles). */
    private fun frac(axis: DoubleArray, x: Double): Double {
        if (x <= axis[0]) return 0.0
        for (k in 0 until axis.size - 1)
            if (x <= axis[k + 1]) return k + (x - axis[k]) / (axis[k + 1] - axis[k])
        return (axis.size - 1).toDouble()
    }

    fun name(code: Int?): String? = code?.let { names[it] }

    /** Couleur qtVlm d'un code ; gris pour « N/A » ou un code inconnu. */
    fun color(code: Int?): Int =
        if (code == null || isNone(code)) NONE_COLOR
        else PALETTE.getOrNull(code - 1) ?: NONE_COLOR

    fun isNone(code: Int) = names[code]?.trim()?.uppercase() == "N/A"

    companion object {
        private const val SIGMA = 1.0     // lissage, en mailles de grille
        private const val R = 3           // rayon de voisinage pris en compte
        const val NONE_COLOR = 0xFF505050.toInt()

        /** Palette des configurations de qtVlm (« Couleurs saildef »), codes 1 à 50.
     *  Écarts voulus, pour la lisibilité sur fond noir (noms CM50) :
     *  code 2 (1R J1) bleu éclairci au lieu de 0000FF,
     *  code 6 (3R J4) cyan très clair au lieu du rouge sombre (confondu avec la layline
     *  bâbord), code 7 (GV C0) vert éclairci au lieu de 008000, code 8 (MTWS) rouge au
     *  lieu du bleu marine. */
        private val PALETTE = intArrayOf(
            0x00FF00, 0x4D8DFF, 0xFFFF00, 0xFF00FF, 0x00FFFF, 0xB4F4FF, 0x4CB050, 0xFF2020, 0x803200, 0x800080,
            0x008080, 0xFF6600, 0x00FF66, 0x6600FF, 0x66FF00, 0xFF0066, 0x0066FF, 0xCC0000, 0x00CC00, 0x0000CC,
            0xCCCC00, 0xCC00CC, 0x00CCCC, 0x990000, 0x009900, 0x000099, 0x999900, 0x990099, 0x009999, 0xFF3300,
            0x33FF00, 0x0033FF, 0xFF0033, 0x00FF33, 0x3300FF, 0xCC3300, 0x33CC00, 0x0033CC, 0xCC0033, 0x00CC33,
            0x3300CC, 0xFF6699, 0x66FF99, 0x9966FF, 0x99FF66, 0xFF9966, 0x6699FF, 0xDD0000, 0x00DD00, 0x0000DD,
        ).map { (0xFF000000.toInt() or it) }.toIntArray()

        private fun cells(line: String) = line.trim().split(';', '\t').map { it.trim() }

        /** Analyse les deux fichiers ; exception avec un message clair si invalides. */
        fun parse(saildef: String, sailselect: String): SailChart {
            val names = mutableMapOf<Int, String>()
            for (raw in saildef.lines()) {
                val l = raw.trim()
                if (l.isEmpty()) continue
                val c = l.split(';', limit = 2)
                val code = c[0].trim().toIntOrNull()
                require(code != null && c.size == 2) { "saildef : ligne invalide « $l »" }
                names[code] = c[1].trim()
            }
            require(names.isNotEmpty()) { "saildef vide" }

            val lines = sailselect.lines().map { it.trim() }.filter { it.isNotEmpty() }
            require(lines.size >= 2) { "sailselect : pas de données" }
            val tws = cells(lines[0]).drop(1).map {
                it.toDoubleOrNull() ?: throw IllegalArgumentException("sailselect : TWS invalide « $it »")
            }.toDoubleArray()
            val twa = ArrayList<Double>()
            val grid = ArrayList<IntArray>()
            for (l in lines.drop(1)) {
                val c = cells(l)
                require(c.size == tws.size + 1) { "sailselect : nombre de colonnes incohérent (« ${l.take(30)}… »)" }
                twa += c[0].toDoubleOrNull() ?: throw IllegalArgumentException("sailselect : TWA invalide « ${c[0]} »")
                grid += IntArray(tws.size) { k ->
                    c[k + 1].toIntOrNull() ?: throw IllegalArgumentException("sailselect : code invalide « ${c[k + 1]} »")
                }
            }
            return SailChart(names, twa.toDoubleArray(), tws, grid.toTypedArray())
        }

        /** Luminance (0–1) pour choisir un texte noir ou blanc sur une couleur. */
        fun luminance(c: Int): Double {
            val r = (c shr 16 and 0xFF) / 255.0; val g = (c shr 8 and 0xFF) / 255.0; val b = (c and 0xFF) / 255.0
            return 0.299 * r + 0.587 * g + 0.114 * b
        }
    }
}
