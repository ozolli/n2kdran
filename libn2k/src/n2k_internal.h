/* n2k_internal.h — structures internes de libn2k (non exposées). */
#ifndef N2K_INTERNAL_H
#define N2K_INTERNAL_H

#include "n2k.h"

#define N2K_FP_SLOTS   16      /* réassemblages fast-packet simultanés */
#define N2K_FP_MAX     223     /* taille max d'un message fast-packet */
#define N2K_FP_TIMEOUT 750     /* ms max entre première et dernière trame */

typedef struct {
    int      used;
    int      pgn, src, seq;
    int      total;            /* longueur annoncée par la trame 0 */
    int      got;              /* octets reçus */
    int      next;             /* index de la prochaine trame attendue */
    uint64_t t0;               /* réception de la trame 0 */
    uint8_t  buf[N2K_FP_MAX];
} n2k_fp_slot;

typedef struct {
    int      set;
    double   v;
    uint64_t t;
    int      src, pgn;
} n2k_item_slot;

/* Moyenne glissante par tranches d'une seconde (deux composantes). */
#define N2K_AVG_SECONDS 1800
typedef struct {
    uint64_t sec[N2K_AVG_SECONDS];     /* seconde absolue de la tranche (+1, 0 = vide) */
    double   a[N2K_AVG_SECONDS], b[N2K_AVG_SECONDS];
    uint32_t n[N2K_AVG_SECONDS];
} n2k_avg;

/* Estimation de la dérive par amure (moyennes exponentielles). */
typedef struct {
    double   e, n;                     /* courant calculé (m/s), composantes E/N */
    double   s, c;                     /* cap moyen (sin, cos) */
    double   b;                        /* BSP moyenne (m/s) */
    double   dur;                      /* durée accumulée sur cette amure (s) */
    uint64_t last;                     /* dernier échantillon (ms), 0 = aucun */
} n2k_tack;

#define N2K_LW_SETTLE_MS  30000        /* ignoré après virement / empannage */
#define N2K_LW_MIN_DUR    60.0         /* s de données exigées par amure */
#define N2K_LW_MAX_AGE_MS 1800000      /* au-delà, bord opposé trop ancien */
#define N2K_LW_SIGMA0     (1.0 * 3.14159265358979323846 / 180.0)
#define N2K_LW_MAX        (15.0 * 3.14159265358979323846 / 180.0)
#define N2K_LW_MAX_ROT    (3.0 * 3.14159265358979323846 / 180.0)   /* rad/s */

#define N2K_AIS_MAX     128             /* cibles AIS suivies */
#define N2K_AIS_TTL_MS  360000          /* cible oubliée sans position depuis 6 min */
#define N2K_AIS_TEST_MS 600000          /* balise annoncée en test : ignorée 10 min */
#define N2K_AIS_MIN_REL (0.5 * 1852.0 / 3600.0)   /* vitesse relative minimale (m/s) */

typedef struct {
    uint32_t mmsi;                     /* 0 = case libre ; identifiant d'émetteur si mob */
    int      mob;                      /* 1 : alerte MOB N2K (127233), pas une cible AIS */
    double   lat, lon;                 /* deg */
    int      est;                      /* 1 : position inconnue, prise à celle du bateau à la réception */
    double   cog, sog;                 /* rad, m/s (0 si indisponibles) */
    int      nav_status;               /* 129038 : statut de navigation, -1 inconnu */
    uint64_t test_until;               /* 129802 « … TEST » : balise en test jusqu'à cette date */
    uint64_t t;                        /* réception de la position, 0 = jamais */
    char     name[N2K_AIS_NAME];
    uint64_t name_t;
} n2k_ais_slot;

#define N2K_TARGET_HYST  0.01          /* hystérésis de l'angle optimum (1 % de VMG) */
#define N2K_RANGE_FRAC   0.99          /* plage VMG : ≥ 99 % de l'optimum */

struct n2k_ctx {
    uint32_t      stale_ms;
    n2k_item_slot items[N2K_ITEM_COUNT];
    n2k_fp_slot   fp[N2K_FP_SLOTS];
    n2k_stats     st;

    /* Données calculées */
    const n2k_polar *polar;
    uint32_t      avg_s, target_s, current_s;
    n2k_avg       tws_avg;             /* a = TWS */
    n2k_avg       twd_avg;             /* a = sin TWD, b = cos TWD */
    n2k_avg       cur_avg;             /* a = composante Est, b = Nord (m/s) */
    n2k_target_state tgt_up, tgt_dn;
    double        tws_target;          /* TWS (m/s) utilisée pour les cibles, < 0 = aucune */

    /* Navigation : numéro du WP de destination (129284) et nom (129285). */
    long          wp_dest;             /* -1 = inconnu */
    char          wp_name[48];
    uint64_t      wp_name_t;           /* réception du nom, 0 = jamais */

    /* Dérive : [allure 0 portant / 1 près][amure 0 bâbord / 1 tribord] */
    n2k_tack      tk[2][2];
    int           tk_allure, tk_tack;  /* allure / amure courantes (-1 = aucune) */
    uint64_t      tk_since;            /* début de l'allure/amure courante (ms) */
    uint64_t      tk_prev;             /* échantillon précédent (ms) */
    int           lw_have[2];          /* λ mesuré, par allure */
    double        lw[2];
    uint64_t      lw_t[2];             /* âge de référence de la mesure */
    double        lw_prior[2], lw_prior_sigma[2];

    n2k_ais_slot  ais[N2K_AIS_MAX];
    int           ais_ignore_still;    /* cible immobile jamais dangereuse */
};

typedef void (*n2k_decoder)(n2k_ctx *ctx, int src, int pgn,
                            const uint8_t *d, int len, uint64_t now);

typedef struct {
    int         pgn;
    int         fast;          /* 1 si transporté en fast-packet */
    int         minlen;        /* longueur minimale exigée par le décodeur */
    n2k_decoder fn;
} n2k_pgn_def;

const n2k_pgn_def *n2k_find_pgn(int pgn);

/* Décodeurs AIS (ais.c). */
void n2k_ais_position(n2k_ctx *ctx, int src, int pgn, const uint8_t *d, int len, uint64_t now);
void n2k_ais_static_a(n2k_ctx *ctx, int src, int pgn, const uint8_t *d, int len, uint64_t now);
void n2k_ais_safety(n2k_ctx *ctx, int src, int pgn, const uint8_t *d, int len, uint64_t now);
void n2k_mob_notification(n2k_ctx *ctx, int src, int pgn, const uint8_t *d, int len, uint64_t now);
void n2k_ais_static_b(n2k_ctx *ctx, int src, int pgn, const uint8_t *d, int len, uint64_t now);

/* Écrit une donnée selon la règle de choix de source : la source/PGN qui
 * possède la donnée la garde tant qu'elle n'est pas périmée ; une autre ne
 * peut prendre le relais qu'après péremption. */
void n2k_set(n2k_ctx *ctx, n2k_item item, double v, int src, int pgn, uint64_t now);

/* Écriture d'une donnée calculée (src N2K_SRC_CALC), sans déclencher de calcul. */
void n2k_set_calc(n2k_ctx *ctx, n2k_item item, double v, uint64_t now);

/* Calculs déclenchés par la mise à jour d'une donnée reçue (derive.c). */
void n2k_derive(n2k_ctx *ctx, n2k_item item, uint64_t now);

#endif
