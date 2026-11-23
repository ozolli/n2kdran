package oz.n2kdran

/** Mise en forme d'une valeur de case. */
enum class Kind {
    SPEED,        // m/s → kn, 1 décimale
    SPEED_ABS,    // idem, valeur absolue (VMG)
    ANGLE_SIDE,   // angle relatif, valeur absolue + point d'amure
    ANGLE,        // angle, valeur absolue
    SIGNED_ANGLE, // angle signé
    ANGLE1,       // angle, 1 décimale
    DIR,          // direction 000–359
    PCT,          // pourcentage entier
    DEPTH,        // m, 1 décimale
    DIST,         // m → NM
    TIME,         // s → m:ss / h:mm
    TEMP,         // K → °C, 1 décimale
    PRESS,        // Pa → hPa entier
    NUM1,         // nombre, 1 décimale
    INT,          // entier
    XTE,          // m si < 0,1 NM, sinon NM (unité affichée adaptée)
    PS,           // deux temps : bords bâbord / tribord jusqu'au WP
    SAIL,         // configuration de voilure recommandée (sail chart qtVlm)
    SAILCHART,    // mini sail chart TWA × TWS, croix à la position actuelle
    AIS,          // cible AIS la plus menaçante : CPA et TCPA
}

/** Une donnée affichable dans une case : clé stable (réglages), libellé, donnée du cœur. */
class CellDef(val key: String, val label: String, val item: String, val unit: String, val kind: Kind)

object Cells {
    val ALL = listOf(
        CellDef("bsp", "BSP", "BSP", "kn", Kind.SPEED),
        CellDef("cible", "CIBLE", "TARGET_BSP", "kn", Kind.SPEED),
        CellDef("sog", "SOG", "SOG", "kn", Kind.SPEED),
        CellDef("vmg", "VMG", "VMG", "kn", Kind.SPEED_ABS),
        CellDef("vmgpct", "VMG", "VMG_PCT", "%", Kind.PCT),
        CellDef("polpct", "POLAIRE", "POLAR_PCT", "%", Kind.PCT),
        CellDef("twa", "TWA", "TWA", "°", Kind.ANGLE_SIDE),
        CellDef("awa", "AWA", "AWA", "°", Kind.ANGLE_SIDE),
        CellDef("tws", "TWS", "TWS", "kn", Kind.SPEED),
        CellDef("aws", "AWS", "AWS", "kn", Kind.SPEED),
        CellDef("twd", "TWD", "TWD", "°", Kind.DIR),
        CellDef("twsavg", "TWS 10'", "TWS_AVG", "kn", Kind.SPEED),
        CellDef("twdavg", "TWD 10'", "TWD_AVG", "°", Kind.DIR),
        CellDef("angopt", "ANGLE OPT", "TARGET_TWA", "°", Kind.ANGLE),
        CellDef("cap", "CAP", "HDG_TRUE", "°", Kind.DIR),
        CellDef("cog", "COG", "COG", "°", Kind.DIR),
        CellDef("gite", "GÎTE", "ROLL", "°", Kind.SIGNED_ANGLE),
        CellDef("tangage", "TANGAGE", "PITCH", "°", Kind.SIGNED_ANGLE),
        CellDef("barre", "BARRE", "RUDDER", "°", Kind.SIGNED_ANGLE),
        CellDef("fond", "FOND", "DEPTH", "m", Kind.DEPTH),
        CellDef("loch", "LOCH", "LOG", "NM", Kind.DIST),
        CellDef("journalier", "JOURNALIER", "TRIP", "NM", Kind.DIST),
        CellDef("courant", "COURANT", "DRIFT", "kn", Kind.SPEED),
        CellDef("dircourant", "DIR. COURANT", "SET", "°", Kind.DIR),
        CellDef("derive", "DÉRIVE", "LEEWAY", "°", Kind.ANGLE1),
        CellDef("teau", "T° EAU", "SEA_TEMP", "°C", Kind.TEMP),
        CellDef("tair", "T° AIR", "AIR_TEMP", "°C", Kind.TEMP),
        CellDef("pression", "PRESSION", "PRESSURE", "hPa", Kind.PRESS),
        CellDef("voile", "VOILE", "", "", Kind.SAIL),
        CellDef("sailchart", "SAIL", "", "", Kind.SAILCHART),
        CellDef("bords", "BORDS", "TTW_PORT", "", Kind.PS),
        CellDef("ttw", "TTW", "TTW", "", Kind.TIME),
        CellDef("dtw", "DTW", "DTW_SAIL", "NM", Kind.DIST),
        CellDef("dtwdirect", "DTW DIRECT", "DTW", "NM", Kind.DIST),
        CellDef("wpt", "WPT", "BTW", "°", Kind.DIR),
        CellDef("vmc", "VMC", "WCV", "kn", Kind.SPEED),
        CellDef("xte", "XTE", "XTE", "NM", Kind.XTE),
        CellDef("sats", "SATELLITES", "GNSS_SATS", "", Kind.INT),
        CellDef("hdop", "HDOP", "HDOP", "", Kind.NUM1),
        CellDef("ais", "AIS", "", "", Kind.AIS),
    )
    private val byKey = ALL.associateBy { it.key }
    fun get(key: String): CellDef = byKey[key] ?: ALL[0]

    /** Colonnes : 0 gauche, 1 droite sans WP, 2 droite avec WP actif. */
    val DEFAULTS = arrayOf(
        listOf("bsp", "cible", "vmgpct", "twa", "tws"),
        listOf("sog", "awa", "aws", "gite", "fond"),
        listOf("bords", "ttw", "dtw", "wpt", "fond"),
    )
}
