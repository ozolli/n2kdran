/*
 * n2k.h — Cœur de décodage NMEA 2000 de n2kdran (C11, libc seule, sans I/O).
 *
 * Usage : on pousse des lignes YDRAW (n2k_feed_line) avec l'heure monotone de
 * réception en millisecondes, et on lit l'état courant (n2k_get / n2k_snapshot).
 *
 * Unités SI partout : rad, rad/s, m, m/s, K, Pa, %. La conversion en unités
 * d'affichage (°, nœuds, °C, hPa) est l'affaire de l'appelant.
 * Angles : caps/routes/relèvements dans [0, 2π) ; angles relatifs au bateau
 * (AWA, TWA, barre, gîte, tangage) signés dans (-π, π], bâbord négatif.
 *
 * Définitions des PGN vérifiées contre canboat (docs/canboat.json).
 */
#ifndef N2K_H
#define N2K_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Données de l'état. L'ordre est stable (index utilisé par n2k_snapshot). */
typedef enum {
    N2K_LAT,            /* deg   129025 */
    N2K_LON,            /* deg   129025 */
    N2K_COG,            /* rad   129026 (référence vraie seulement) */
    N2K_SOG,            /* m/s   129026 */
    N2K_HDG_TRUE,       /* rad   127250 ref vraie */
    N2K_HDG_MAG,        /* rad   127250 ref magnétique */
    N2K_VARIATION,      /* rad   127250 / 127258, est positif */
    N2K_ROT,            /* rad/s 127251 */
    N2K_PITCH,          /* rad   127257 */
    N2K_ROLL,           /* rad   127257 (gîte, signe conservé) */
    N2K_RUDDER,         /* rad   127245 position */
    N2K_BSP,            /* m/s   128259 vitesse surface */
    N2K_DEPTH,          /* m     128267 sous le transducteur */
    N2K_DEPTH_OFFSET,   /* m     128267 (+ : surface, - : quille) */
    N2K_LOG,            /* m     128275 total */
    N2K_TRIP,           /* m     128275 journalier */
    N2K_AWA,            /* rad   130306 ref 2 */
    N2K_AWS,            /* m/s   130306 ref 2 */
    N2K_TWA,            /* rad   130306 ref 4 (vrai, référencé à l'eau) */
    N2K_TWS,            /* m/s   130306 ref 4 */
    N2K_GWA,            /* rad   130306 ref 3 (vrai sol, référencé au bateau) */
    N2K_GWS,            /* m/s   130306 ref 3 ou 0 */
    N2K_GWD,            /* rad   130306 ref 0 (vrai sol, direction / Nord vrai) */
    N2K_SET,            /* rad   129291 (vraie), sinon calculé (src N2K_SRC_CALC) */
    N2K_DRIFT,          /* m/s   129291, sinon calculé */
    N2K_SEA_TEMP,       /* K     130316 / 130312 / 130311, source 0 */
    N2K_AIR_TEMP,       /* K     130316 / 130312 / 130311, source 1 */
    N2K_PRESSURE,       /* Pa    130314 source 0 / 130311 */
    N2K_HUMIDITY,       /* %     130311 */
    N2K_GNSS_METHOD,    /* enum  129029 (0 pas de fix, 1 GNSS, 2 DGNSS, ...) */
    N2K_GNSS_SATS,      /* nb    129029 */
    N2K_HDOP,           /* -     129029 / 129539 */
    N2K_PDOP,           /* -     129029 */
    N2K_XTE,            /* m     129283 (signe tel que reçu) */
    N2K_DTW,            /* m     129284 distance au WP */
    N2K_BTW,            /* rad   129284 relèvement position → WP (vrai) */
    N2K_WCV,            /* m/s   129284 vitesse de rapprochement du WP */

    /* --- Données calculées (src = N2K_SRC_CALC) --- */
    N2K_TWD,            /* rad   cap vrai + TWA */
    N2K_TWS_AVG,        /* m/s   moyenne glissante de TWS (fenêtre avg_s) */
    N2K_TWD_AVG,        /* rad   moyenne vectorielle (unitaire) de TWD (fenêtre avg_s) */
    N2K_VMG,            /* m/s   BSP·cos(TWA) : > 0 au près, < 0 au portant */
    N2K_POLAR_BSP,      /* m/s   vitesse polaire au TWA/TWS instantanés */
    N2K_POLAR_PCT,      /* %     BSP / vitesse polaire */
    N2K_TARGET_TWA,     /* rad   angle optimum stabilisé de l'allure courante (> 0) */
    N2K_TARGET_BSP,     /* m/s   vitesse cible à cet angle */
    N2K_VMG_PCT,        /* %     |VMG| / VMG cible */
    N2K_VMG_RANGE_MIN,  /* rad   plage de TWA à VMG ≥ 99 % de l'optimum (> 0) */
    N2K_VMG_RANGE_MAX,  /* rad */
    N2K_TWA_VMG_PCT,    /* %     VMG polaire au TWA actuel / VMG optimum (même TWS
                         *       moyennée) : 100 sur l'optimum, ≥ 99 dans la plage */

    /* Laylines (caps vrais à tenir sur chaque amure, sans correction de
     * courant ni de dérive), pour l'allure courante, avec l'angle stabilisé.
     * Tribord amures = vent reçu à tribord (TWA > 0). Les secteurs d'historique
     * vont de MIN à MAX dans le sens horaire (MIN > MAX numériquement si le
     * secteur passe par le Nord). */
    N2K_TWD_MIN,        /* rad   TWD min sur avg_s (écart à la moyenne, Nord géré) */
    N2K_TWD_MAX,        /* rad   TWD max sur avg_s */
    N2K_LAY_STBD,       /* rad   TWD − angle optimum */
    N2K_LAY_PORT,       /* rad   TWD + angle optimum */
    N2K_LAY_STBD_MIN,   /* rad   TWD_MIN − angle optimum */
    N2K_LAY_STBD_MAX,   /* rad   TWD_MAX − angle optimum */
    N2K_LAY_PORT_MIN,   /* rad   TWD_MIN + angle optimum */
    N2K_LAY_PORT_MAX,   /* rad   TWD_MAX + angle optimum */

    /* Laylines FOND : route fond sur chaque amure = vecteur (vitesse cible, cap
     * de layline) + vecteur courant (SET/DRIFT : instrument, sinon calculé).
     * Le courant calculé contient la dérive de l'amure où il a été mesuré :
     * sur l'autre amure, l'erreur est d'environ deux fois l'angle de dérive. */
    N2K_LAY_STBD_GND,     /* rad */
    N2K_LAY_PORT_GND,     /* rad */
    N2K_LAY_STBD_GND_MIN, /* rad   bornes des secteurs d'historique, corrigées */
    N2K_LAY_STBD_GND_MAX, /* rad */
    N2K_LAY_PORT_GND_MIN, /* rad */
    N2K_LAY_PORT_GND_MAX, /* rad */

    /* Dérive (leeway) estimée et incertitude des laylines fond.
     * Sur chaque amure, courant calculé = courant réel + dérive (λ·BSP, par le
     * travers sous le vent). Après un bord de chaque côté (même allure), la
     * différence entre les deux amures donne λ, puis le courant réel. Avec un
     * courant fourni par un instrument, λ se mesure sur une seule amure.
     * L'incertitude σ croît avec l'âge du bord opposé ; sans mesure, λ et σ
     * valent la valeur a priori (n2k_set_leeway_prior).
     * Les laylines fond (LAY_*_GND) utilisent λ ; LO / HI sont les mêmes
     * laylines calculées avec λ − σ et λ + σ (bords d'une bande d'incertitude,
     * pas forcément dans le sens horaire). Sur l'amure courante la bande est
     * quasi nulle quand le courant est calculé : il est mesuré sur cette amure. */
    N2K_LEEWAY,           /* rad   λ mesuré (absent tant qu'il n'est pas mesuré) */
    N2K_LEEWAY_SIGMA,     /* rad   incertitude utilisée pour les bandes */
    N2K_LAY_STBD_GND_LO,  /* rad */
    N2K_LAY_STBD_GND_HI,  /* rad */
    N2K_LAY_PORT_GND_LO,  /* rad */
    N2K_LAY_PORT_GND_HI,  /* rad */

    /* Route vers le WP (129284 : distance et relèvement) en tirant des bords sur
     * les laylines fond de l'allure courante : WP = a·(layline tribord) +
     * b·(layline bâbord). Temps = distance / vitesse fond du bord (vitesse cible
     * + courant + dérive estimée). WP hors du cône d'un côté : faisable
     * directement sur une amure (l'autre à 0). Absents si le WP est derrière
     * les deux laylines (autre allure). */
    N2K_TTW_STBD,         /* s     temps à passer tribord amures */
    N2K_TTW_PORT,         /* s     temps à passer bâbord amures */
    N2K_TTW,              /* s     temps total jusqu'au WP */
    N2K_DTW_SAIL,         /* m     distance totale parcourue (a + b) */

    /* Route fond sur l'amure opposée en gardant le TWA actuel (même |TWA|, vitesse
     * polaire à cet angle, même courant et même dérive que les laylines fond) :
     * avec LAY_*_GND de l'amure opposée, borne le choix d'angle après virement. */
    N2K_OPP_TWA_GND,      /* rad */
    N2K_ITEM_COUNT
} n2k_item;

typedef enum {
    N2K_NEVER = 0,      /* jamais reçu */
    N2K_STALE = 1,      /* reçu, mais plus vieux que le délai de péremption */
    N2K_OK    = 2       /* valeur fraîche */
} n2k_status;

typedef struct {
    uint64_t lines;         /* lignes reçues */
    uint64_t bad_lines;     /* lignes rejetées par le parseur */
    uint64_t frames_used;   /* trames d'un PGN décodé */
    uint64_t frames_ignored;/* trames d'un PGN non décodé (AIS, ...) */
    uint64_t fp_complete;   /* fast-packets réassemblés */
    uint64_t fp_dropped;    /* fast-packets abandonnés (trame perdue, séquence...) */
    uint64_t decode_errors; /* message trop court pour son PGN */
} n2k_stats;

typedef struct n2k_ctx n2k_ctx;
typedef struct n2k_polar n2k_polar;

/* Pseudo-adresse source des données calculées par le cœur. Une donnée calculée
 * ne remplace jamais une donnée fraîche reçue d'un instrument ; une donnée
 * d'instrument remplace toujours une donnée calculée. */
#define N2K_SRC_CALC (-1)

/* Création / destruction (une seule allocation). NULL si plus de mémoire. */
n2k_ctx *n2k_create(void);
void     n2k_destroy(n2k_ctx *ctx);

/* Délai au-delà duquel une donnée est périmée (défaut 3000 ms). C'est aussi le
 * délai après lequel une autre source peut prendre le relais. Les données de
 * navigation (129283/129284/129285 : 1 Hz, souvent en UDP) ont au moins 10 s. */
void n2k_set_stale_ms(n2k_ctx *ctx, uint32_t ms);

/* Polaire utilisée pour les données calculées (NULL pour l'enlever). Le
 * contexte ne la copie pas : elle doit rester valide tant qu'elle est installée. */
void n2k_set_polar(n2k_ctx *ctx, const n2k_polar *polar);

/* Fenêtres de moyenne, en secondes (≤ 1800) : avg_s pour TWS_AVG/TWD_AVG
 * (défaut 600), target_s pour la TWS qui sert aux cibles (défaut 60),
 * current_s pour le courant calculé (défaut 60). 0 = inchangé. */
void n2k_set_windows(n2k_ctx *ctx, uint32_t avg_s, uint32_t target_s, uint32_t current_s);

/* Dérive a priori (rad) au près et au portant, utilisée tant qu'elle n'est pas
 * mesurée, et incertitude maximale associée (défauts : 4° ± 4° au près,
 * 2° ± 2° au portant — valeurs génériques, à ajuster au bateau). */
void n2k_set_leeway_prior(n2k_ctx *ctx, double up, double up_sigma,
                          double down, double down_sigma);

/* Profil de VMG pour l'affichage : pct[k] = VMG polaire au TWA k° (0..180)
 * en % du VMG optimum de son allure (près si k < 90, portant sinon), à la TWS
 * utilisée pour les cibles. Valeurs ≤ 100, négatives ramenées à 0. Retourne 1
 * si disponible (polaire installée et TWS reçue), 0 sinon. */
int n2k_vmg_profile(const n2k_ctx *ctx, double pct[181]);

/* Nom du WP de destination (129285, route/WP information), en UTF-8, terminé
 * par NUL. Retourne le statut (N2K_NEVER si jamais reçu ; N2K_STALE si plus
 * vieux que le délai de péremption). */
n2k_status n2k_wp_name(const n2k_ctx *ctx, uint64_t now_ms, char *buf, size_t size);

/* Pousse une ligne YDRAW (avec ou sans CR/LF final). `now_ms` = horloge
 * monotone de l'appelant. Retourne 1 si la ligne est valide, 0 sinon. Ne lit
 * jamais au-delà du NUL final ; toute entrée est tolérée. */
int n2k_feed_line(n2k_ctx *ctx, const char *line, uint64_t now_ms);

/* Variante avec options. N2K_FEED_NAV_ONLY : ne retient que la navigation
 * (129283 XTE, 129284 WP, 129285 noms de route/WP) — pour un flux secondaire (qtVlm) qui réémet aussi
 * les données du bateau, à ne pas mélanger avec les instruments. */
#define N2K_FEED_NAV_ONLY 1u
int n2k_feed_line_ex(n2k_ctx *ctx, const char *line, uint64_t now_ms, unsigned flags);

/* Pousse une trame CAN déjà décodée (utilisé par n2k_feed_line, exposé pour
 * les tests et d'éventuelles autres sources). */
void n2k_feed_frame(n2k_ctx *ctx, uint32_t can_id, const uint8_t *data, int len,
                    uint64_t now_ms);

/* Lit une donnée. *value n'est écrit que si le statut n'est pas N2K_NEVER. */
n2k_status n2k_get(const n2k_ctx *ctx, n2k_item item, uint64_t now_ms, double *value);

/* Provenance d'une donnée : adresse source, PGN, âge en ms. Retourne le statut. */
n2k_status n2k_get_info(const n2k_ctx *ctx, n2k_item item, uint64_t now_ms,
                        int *src, int *pgn, uint64_t *age_ms);

/* Lecture groupée de tout l'état (pour JNI) : `values` et `status` doivent
 * contenir N2K_ITEM_COUNT éléments. Valeur NaN si jamais reçue. */
void n2k_snapshot(const n2k_ctx *ctx, uint64_t now_ms, double *values, uint8_t *status);

void        n2k_get_stats(const n2k_ctx *ctx, n2k_stats *out);
const char *n2k_item_name(n2k_item item);

/* --- AIS ---
 * Cibles reçues en 129038 / 129039 (position), 129794 / 129809 (nom). Une cible
 * sans position depuis N2K_AIS_TTL_MS est oubliée. Calculs dans un plan local
 * autour du bateau (précis à quelques dizaines de milles). */
#define N2K_AIS_NAME 21

typedef struct {
    uint32_t mmsi;
    char     name[N2K_AIS_NAME]; /* ASCII, "" si inconnu */
    double   range;              /* m, distance actuelle */
    double   bearing;            /* rad, relèvement vrai de la cible [0, 2π) */
    double   cpa;                /* m, distance au point de rapprochement maximal */
    double   tcpa;               /* s, temps jusqu'à ce point (< 0 : déjà passé) */
    double   rel_speed;          /* m/s, vitesse relative */
    uint64_t age_ms;             /* âge de la dernière position */
    int      est;                /* 1 : position inconnue, estimée à celle du bateau
                                    quand l'alerte a été reçue (MOB sans point GNSS) */
} n2k_ais_target;

/* Cible la plus menaçante : parmi les cibles qui se rapprochent (TCPA entre 0
 * et tcpa_max s, vitesse relative ≥ 0,5 nœud), celle de plus petit TCPA dont le
 * CPA est sous cpa_max m (retour 2, dangereuse), sinon celle de plus petit CPA
 * (retour 1). 0 : aucune, ou position du bateau absente. Cibles à plus de
 * range_max m ignorées (≤ 0 : pas de limite), ainsi que les nskip MMSI de
 * skip (alertes acquittées : on passe à la cible suivante). Balises de détresse
 * exclues (cf. n2k_ais_distress). Notre vitesse : COG/SOG (nulle si absents). */
int n2k_ais_threat(const n2k_ctx *ctx, uint64_t now_ms, double cpa_max, double tcpa_max,
                   double range_max, const uint32_t *skip, int nskip, n2k_ais_target *out);

/* on ≠ 0 : une cible immobile (vitesse fond sous 0,5 nœud, ou inconnue) n'est
 * jamais dangereuse : n2k_ais_threat la classe au plus en 1 (suivie, sans
 * alarme), même si le bateau se dirige vers elle. Défaut : 0. */
void n2k_set_ais_ignore_still(n2k_ctx *ctx, int on);

/* Balise de détresse active, homme à la mer d'abord, puis la plus proche : retourne le préfixe du MMSI (970
 * AIS-SART, 972 MOB, 974 EPIRB-AIS), ou N2K_MOB_N2K pour une alerte homme à la
 * mer reçue en 127233 (mmsi = identifiant de l'émetteur), 0 si aucune. Balises en
 * mode test ignorées. Sans position du bateau ou de l'alerte : range, bearing = NaN. */
#define N2K_MOB_N2K 127233
/* Un homme à la mer (MOB AIS 972… actif ou 127233 actif) reste signalé jusqu'à
 * n2k_ais_reset, même sans nouveau message ou avec un statut inactif ensuite. */
void n2k_ais_reset(n2k_ctx *ctx);
int n2k_ais_distress(const n2k_ctx *ctx, uint64_t now_ms, const uint32_t *skip, int nskip,
                     n2k_ais_target *out);

/* Nombre de cibles AIS suivies (position de moins de N2K_AIS_TTL_MS). */
int n2k_ais_count(const n2k_ctx *ctx, uint64_t now_ms);

/* --- Polaire (format texte qtVlm) ---
 * Première ligne : `TWA\TWS` puis les TWS (nœuds) ; lignes suivantes : TWA (°)
 * puis BSP (nœuds). Séparateur `;` ou tabulation, LF ou CRLF.
 * Pas d'I/O : l'appelant lit le fichier et passe son contenu.
 * Interpolation bilinéaire ; |TWA| (polaire symétrique). Au-delà de la dernière
 * colonne TWS non entièrement nulle, la TWS est bornée à cette colonne (évite
 * d'interpoler vers une colonne finale de zéros). En dessous de la première
 * TWS / TWA connue, interpolation vers 0 (BSP nulle à TWS 0 et à TWA 0). */

/* Analyse `text` (`len` octets, pas forcément terminé par NUL). NULL en cas
 * d'erreur, avec un message dans `err` (si non NULL). */
n2k_polar *n2k_polar_parse(const char *text, size_t len, char *err, size_t errsz);
void       n2k_polar_free(n2k_polar *p);

/* Vitesse polaire (m/s) pour un TWA (rad, signe ignoré) et une TWS (m/s). */
double n2k_polar_speed(const n2k_polar *p, double twa, double tws);

/* Angle de VMG optimum (rad, positif) et vitesse cible (m/s) pour une TWS (m/s),
 * au près (upwind = 1, TWA 0–90°) ou au portant (upwind = 0, 90–180°).
 * Recherche par pas de 0,1° (comme vmg_optimal_angles de polar_doctor). Retourne 1 si trouvé, 0 sinon (VMG nul partout). */
int n2k_polar_target(const n2k_polar *p, double tws, int upwind,
                     double *twa_opt, double *bsp_target);

/* Plage de TWA (rad, positifs) où le VMG atteint au moins `frac` (ex. 0,99) du
 * VMG optimum, au près ou au portant. Si la courbe de VMG a plusieurs pics
 * (polaire plate ou à coude), la plage est l'enveloppe de toutes les zones
 * au-dessus du seuil. Retourne 1 si trouvée, 0 sinon. */
int n2k_polar_vmg_range(const n2k_polar *p, double tws, int upwind, double frac,
                        double *twa_min, double *twa_max);

/* Angle optimum stabilisé (hystérésis), pour éviter les bascules entre deux
 * pics de VMG voisins. L'état est conservé par l'appelant (un par allure),
 * initialisé à zéro. À chaque appel : on suit, par montée de gradient depuis
 * l'angle précédent, le sommet du pic de VMG courant ; on ne bascule sur l'optimum global que s'il
 * dépasse ce maximum local de plus de `hyst` (ex. 0,01 = 1 %).
 * Pensé pour être appelé avec une TWS moyennée. */
typedef struct {
    int    valid;
    double twa;          /* rad, angle retenu au dernier appel */
} n2k_target_state;

int n2k_polar_target_stable(const n2k_polar *p, double tws, int upwind, double hyst,
                            n2k_target_state *st, double *twa_opt, double *bsp_target);

/* --- Utilitaires exposés pour les tests --- */

/* Analyse une ligne YDRAW. Retourne 1 si valide (id ≤ 29 bits, 0..8 octets). */
int n2k_parse_ydraw(const char *line, uint32_t *tod_ms, uint32_t *can_id,
                    uint8_t data[8], int *len);

/* Décode l'identifiant CAN 29 bits (PDU1 : dst = PS ; PDU2 : dst = 255). */
void n2k_decode_id(uint32_t can_id, int *prio, int *pgn, int *src, int *dst);

#ifdef __cplusplus
}
#endif

#endif /* N2K_H */
