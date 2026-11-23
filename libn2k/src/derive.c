/*
 * derive.c — Données calculées par le cœur, déclenchées par la réception.
 *
 * - TWA/TWS (vent vrai, référencé à l'eau, fourni par le Madintec : jamais
 *   recalculé ; déclenché à l'écriture de la TWS, qui suit le TWA dans 130306)
 *     → TWD = cap vrai + TWA, moyenne vectorielle de TWD,
 *       VMG, vitesse et % polaire, cibles (sur la TWS moyennée sur target_s),
 *       plage de VMG.
 * - TWS → moyenne glissante (avant les calculs ci-dessus).
 *   Laylines : TWD ± angle optimum, et secteurs d'historique sur avg_s ;
 *   laylines fond = vitesse cible au cap de layline + courant.
 * - SOG (avec COG) → courant calculé si aucun instrument ne fournit 129291 :
 *       (SOG, COG) − (BSP, cap vrai), moyenné vectoriellement sur current_s.
 *       Sans dérive connue, ce « courant » inclut la dérive (leeway).
 *
 * Une donnée calculée n'est écrite que si toutes ses entrées sont fraîches :
 * sinon elle cesse d'être rafraîchie et devient périmée d'elle-même.
 */
#include "n2k_internal.h"

#include <math.h>

#define PI     3.14159265358979323846
#define TWO_PI (2.0 * PI)

static double wrap_2pi(double a)
{
    a = fmod(a, TWO_PI);
    return a < 0 ? a + TWO_PI : a;
}

/* Valeur fraîche d'une donnée de l'état. */
static int fresh(const n2k_ctx *ctx, n2k_item it, uint64_t now, double *v)
{
    return n2k_get(ctx, it, now, v) == N2K_OK;
}

/* --- Moyenne par tranches d'une seconde --- */

static void avg_push(n2k_avg *g, uint64_t now, double a, double b)
{
    uint64_t sec = now / 1000u + 1u;            /* +1 : 0 réservé à « vide » */
    size_t i = (size_t)(sec % N2K_AVG_SECONDS);
    if (g->sec[i] != sec) {
        g->sec[i] = sec;
        g->a[i] = g->b[i] = 0;
        g->n[i] = 0;
    }
    g->a[i] += a;
    g->b[i] += b;
    g->n[i]++;
}

/* Moyenne des échantillons des `window` dernières secondes (seconde courante
 * comprise). Retourne le nombre d'échantillons. */
static uint32_t avg_get(const n2k_avg *g, uint64_t now, uint32_t window, double *a, double *b)
{
    uint64_t sec = now / 1000u + 1u;
    double sa = 0, sb = 0;
    uint32_t n = 0;
    for (uint32_t k = 0; k < window && k < sec; k++) {
        size_t i = (size_t)((sec - k) % N2K_AVG_SECONDS);
        if (g->sec[i] == sec - k) {
            sa += g->a[i];
            sb += g->b[i];
            n += g->n[i];
        }
    }
    if (n) {
        *a = sa / n;
        *b = sb / n;
    }
    return n;
}

/* Angle ramené dans (-π, π]. */
static double wrap_pi(double a)
{
    a = wrap_2pi(a);
    return a > PI ? a - TWO_PI : a;
}

/* Direction moyenne et écarts min/max (rad) des moyennes par seconde d'un
 * moyenneur de directions (a = Σ sin, b = Σ cos) sur `window` secondes.
 * Les écarts sont pris par rapport à la moyenne : le Nord n'est pas un bord. */
static int dir_minmax(const n2k_avg *g, uint64_t now, uint32_t window,
                      double *mean, double *lo, double *hi)
{
    double x, y;
    if (!avg_get(g, now, window, &x, &y) || (x == 0 && y == 0))
        return 0;
    double m = atan2(x, y), dmin = 0, dmax = 0;
    uint64_t sec = now / 1000u + 1u;
    for (uint32_t k = 0; k < window && k < sec; k++) {
        size_t i = (size_t)((sec - k) % N2K_AVG_SECONDS);
        if (g->sec[i] != sec - k || (g->a[i] == 0 && g->b[i] == 0))
            continue;
        double d = wrap_pi(atan2(g->a[i], g->b[i]) - m);
        if (d < dmin) dmin = d;
        if (d > dmax) dmax = d;
    }
    *mean = wrap_2pi(m);
    *lo = wrap_2pi(m + dmin);
    *hi = wrap_2pi(m + dmax);
    return 1;
}

/* --- Vent --- */

static void derive_laylines(n2k_ctx *ctx, uint64_t now);

static void derive_wind(n2k_ctx *ctx, uint64_t now)
{
    double twa, tws, hdg, bsp, x, y;
    if (!fresh(ctx, N2K_TWA, now, &twa))
        return;

    if (fresh(ctx, N2K_HDG_TRUE, now, &hdg)) {
        double twd = wrap_2pi(hdg + twa);
        n2k_set_calc(ctx, N2K_TWD, twd, now);
        avg_push(&ctx->twd_avg, now, sin(twd), cos(twd));
        if (avg_get(&ctx->twd_avg, now, ctx->avg_s, &x, &y) && (x != 0 || y != 0))
            n2k_set_calc(ctx, N2K_TWD_AVG, wrap_2pi(atan2(x, y)), now);
    }

    if (!fresh(ctx, N2K_TWS, now, &tws) || !fresh(ctx, N2K_BSP, now, &bsp))
        return;
    double vmg = bsp * cos(twa);
    n2k_set_calc(ctx, N2K_VMG, vmg, now);

    const n2k_polar *p = ctx->polar;
    if (!p)
        return;
    double pol = n2k_polar_speed(p, twa, tws);
    if (isfinite(pol)) {
        n2k_set_calc(ctx, N2K_POLAR_BSP, pol, now);
        if (pol > 0.05)                         /* évite les % absurdes vent nul */
            n2k_set_calc(ctx, N2K_POLAR_PCT, 100.0 * bsp / pol, now);
    }

    /* Cibles : TWS moyennée sur target_s, allure selon le TWA courant. */
    double tws_t;
    if (!avg_get(&ctx->tws_avg, now, ctx->target_s, &tws_t, &x))
        tws_t = tws;
    ctx->tws_target = tws_t;
    int upwind = fabs(twa) < PI / 2;
    n2k_target_state *st = upwind ? &ctx->tgt_up : &ctx->tgt_dn;
    double t_twa, t_bsp, lo, hi;
    if (n2k_polar_target_stable(p, tws_t, upwind, N2K_TARGET_HYST, st, &t_twa, &t_bsp)) {
        n2k_set_calc(ctx, N2K_TARGET_TWA, t_twa, now);
        n2k_set_calc(ctx, N2K_TARGET_BSP, t_bsp, now);
        double t_vmg = fabs(t_bsp * cos(t_twa));
        if (t_vmg > 0.05)
            n2k_set_calc(ctx, N2K_VMG_PCT, 100.0 * fabs(vmg) / t_vmg, now);
    }
    /* Qualité de l'angle seul : VMG polaire au TWA actuel / VMG optimum. */
    double g_twa, g_bsp;
    if (n2k_polar_target(p, tws_t, upwind, &g_twa, &g_bsp)) {
        double sgn = upwind ? 1.0 : -1.0;
        double best = g_bsp * cos(g_twa) * sgn;
        double here = n2k_polar_speed(p, twa, tws_t) * cos(twa) * sgn;
        if (best > 0.05 && isfinite(here))
            n2k_set_calc(ctx, N2K_TWA_VMG_PCT, 100.0 * here / best, now);
    }
    if (n2k_polar_vmg_range(p, tws_t, upwind, N2K_RANGE_FRAC, &lo, &hi)) {
        n2k_set_calc(ctx, N2K_VMG_RANGE_MIN, lo, now);
        n2k_set_calc(ctx, N2K_VMG_RANGE_MAX, hi, now);
    }
    derive_laylines(ctx, now);
}

/* Route fond pour un cap `h` à la vitesse surface `v`, avec le courant (e, n). */
static double ground_course(double h, double v, double ce, double cn)
{
    return wrap_2pi(atan2(v * sin(h) + ce, v * cos(h) + cn));
}

/* Courant à utiliser pour les laylines fond : composantes Est/Nord (m/s).
 * DRIFT frais mais SET non rafraîchi = courant quasi nul (direction indéfinie). */
static int current_vec(const n2k_ctx *ctx, uint64_t now, double *ce, double *cn)
{
    double set, drift;
    if (!fresh(ctx, N2K_DRIFT, now, &drift))
        return 0;
    if (fresh(ctx, N2K_SET, now, &set)) {
        *ce = drift * sin(set);
        *cn = drift * cos(set);
        return 1;
    }
    if (drift <= 0.02) {
        *ce = *cn = 0;
        return 1;
    }
    return 0;
}

/* Route vers le WP en deux bords : WP = a·us + b·up (us, up : directions fond
 * unitaires des deux bords), temps = a / vitesse fond tribord + b / vitesse
 * fond bâbord. WP hors du cône d'un côté : faisable directement sur une amure
 * (temps de l'autre = 0), au rythme de la VMC réelle (WCV, comme qtVlm), sinon
 * de la layline ; rien si le WP est derrière les deux laylines. */
static void route_to_wp(n2k_ctx *ctx, uint64_t now, double se, double sn, double pe, double pn)
{
    double dtw, btw;
    if (!fresh(ctx, N2K_DTW, now, &dtw) || !fresh(ctx, N2K_BTW, now, &btw))
        return;
    double vs = sqrt(se * se + sn * sn), vp = sqrt(pe * pe + pn * pn);
    if (vs < 0.1 || vp < 0.1)
        return;
    double use = se / vs, usn = sn / vs, upe = pe / vp, upn = pn / vp;
    double we = dtw * sin(btw), wn = dtw * cos(btw);
    double det = use * upn - usn * upe;
    if (fabs(det) < 1e-3)
        return;                                 /* bords quasi parallèles */
    double a = (we * upn - wn * upe) / det;
    double b = (use * wn - usn * we) / det;
    if (a < -1.0 && b < -1.0)                   /* WP derrière les deux laylines */
        return;
    /* Un seul bord négatif : le WP est hors du cône du côté de l'autre amure,
     * donc faisable directement sur celle-ci (sur la layline ou au-delà). */
    if (b < -1.0 || a < -1.0) {                 /* route directe : temps = DTW / VMC */
        double vmc;
        int ok = fresh(ctx, N2K_WCV, now, &vmc) && vmc > 0.1;
        if (b < -1.0) { a = dtw; b = 0; if (ok) vs = vmc; }
        else          { b = dtw; a = 0; if (ok) vp = vmc; }
    }
    if (a < 0) a = 0;
    if (b < 0) b = 0;
    n2k_set_calc(ctx, N2K_TTW_STBD, a / vs, now);
    n2k_set_calc(ctx, N2K_TTW_PORT, b / vp, now);
    n2k_set_calc(ctx, N2K_TTW, a / vs + b / vp, now);
    n2k_set_calc(ctx, N2K_DTW_SAIL, a + b, now);
}

/* Laylines : TWD frais (calculé juste avant) et angle optimum de l'allure. */
static void derive_laylines(n2k_ctx *ctx, uint64_t now)
{
    double twd, opt, m, lo = 0, hi = 0, tbsp, ce = 0, cn = 0;
    if (!fresh(ctx, N2K_TWD, now, &twd) || !fresh(ctx, N2K_TARGET_TWA, now, &opt))
        return;
    n2k_set_calc(ctx, N2K_LAY_STBD, wrap_2pi(twd - opt), now);
    n2k_set_calc(ctx, N2K_LAY_PORT, wrap_2pi(twd + opt), now);
    int hist = dir_minmax(&ctx->twd_avg, now, ctx->avg_s, &m, &lo, &hi);
    if (hist) {
        n2k_set_calc(ctx, N2K_TWD_MIN, lo, now);
        n2k_set_calc(ctx, N2K_TWD_MAX, hi, now);
        n2k_set_calc(ctx, N2K_LAY_STBD_MIN, wrap_2pi(lo - opt), now);
        n2k_set_calc(ctx, N2K_LAY_STBD_MAX, wrap_2pi(hi - opt), now);
        n2k_set_calc(ctx, N2K_LAY_PORT_MIN, wrap_2pi(lo + opt), now);
        n2k_set_calc(ctx, N2K_LAY_PORT_MAX, wrap_2pi(hi + opt), now);
    }

    /* Laylines fond : route surface (cap ∓ dérive) à la vitesse cible + courant. */
    if (!fresh(ctx, N2K_TARGET_BSP, now, &tbsp) || tbsp < 0.1)
        return;
    int al = opt < PI / 2;                      /* allure de l'angle optimum */

    /* Dérive : mesurée (incertitude croissant avec l'âge) ou a priori. */
    double lam = ctx->lw_prior[al], sig = ctx->lw_prior_sigma[al];
    if (ctx->lw_have[al]) {
        double age = (double)(now > ctx->lw_t[al] ? now - ctx->lw_t[al] : 0);
        double f = age / N2K_LW_MAX_AGE_MS;
        if (f > 1) f = 1;
        lam = ctx->lw[al];
        sig = N2K_LW_SIGMA0 + (ctx->lw_prior_sigma[al] - N2K_LW_SIGMA0) * f;
        if (sig < N2K_LW_SIGMA0) sig = N2K_LW_SIGMA0;
        n2k_set_calc(ctx, N2K_LEEWAY, lam, now);
    }
    n2k_set_calc(ctx, N2K_LEEWAY_SIGMA, sig, now);

    /* Courant réel : instrument, sinon courant calculé de l'amure courante
     * corrigé de sa dérive (dépend de λ), sinon courant calculé brut. */
    int src, mode;
    const n2k_tack *k = NULL;
    double we = 0, wn = 0;
    if (n2k_get_info(ctx, N2K_SET, now, &src, NULL, NULL) == N2K_OK && src != N2K_SRC_CALC &&
        current_vec(ctx, now, &ce, &cn)) {
        mode = 0;
    } else if (ctx->tk_allure == al && ctx->tk_tack >= 0 &&
               (k = &ctx->tk[al][ctx->tk_tack])->last && now - k->last < 5000 &&
               k->dur >= 10.0) {
        mode = 1;
        double h = atan2(k->s, k->c) + (ctx->tk_tack ? -PI / 2 : PI / 2);
        we = sin(h);                            /* travers sous le vent */
        wn = cos(h);
    } else if (current_vec(ctx, now, &ce, &cn)) {
        mode = 0;
    } else {
        return;
    }

    double lams[3] = { lam, lam - sig, lam + sig };
    for (int i = 0; i < 3; i++) {
        double l = lams[i] < 0 ? 0 : (lams[i] > N2K_LW_MAX ? N2K_LW_MAX : lams[i]);
        double e = ce, n = cn;
        if (mode == 1) {
            e = k->e - l * k->b * we;
            n = k->n - l * k->b * wn;
        }
        double gs = ground_course(twd - opt - l, tbsp, e, n);   /* tribord : sous le vent = bâbord */
        double gp = ground_course(twd + opt + l, tbsp, e, n);
        static const n2k_item S[3] = { N2K_LAY_STBD_GND, N2K_LAY_STBD_GND_LO, N2K_LAY_STBD_GND_HI };
        static const n2k_item P[3] = { N2K_LAY_PORT_GND, N2K_LAY_PORT_GND_LO, N2K_LAY_PORT_GND_HI };
        n2k_set_calc(ctx, S[i], gs, now);
        n2k_set_calc(ctx, P[i], gp, now);
        if (i == 0) {
            /* Vecteurs fond de chaque bord, pour la route vers le WP. */
            double ts = twd - opt - l, tp = twd + opt + l;
            route_to_wp(ctx, now, tbsp * sin(ts) + e, tbsp * cos(ts) + n,
                        tbsp * sin(tp) + e, tbsp * cos(tp) + n);
            /* Amure opposée au même TWA (TWA > 0 : tribord amures → bâbord). */
            double twa, pb;
            if (fresh(ctx, N2K_TWA, now, &twa) && fresh(ctx, N2K_POLAR_BSP, now, &pb) && pb >= 0.1) {
                double h = twa > 0 ? twd + fabs(twa) + l : twd - fabs(twa) - l;
                n2k_set_calc(ctx, N2K_OPP_TWA_GND, ground_course(h, pb, e, n), now);
            }
        }
        if (i == 0 && hist) {
            n2k_set_calc(ctx, N2K_LAY_STBD_GND_MIN, ground_course(lo - opt - l, tbsp, e, n), now);
            n2k_set_calc(ctx, N2K_LAY_STBD_GND_MAX, ground_course(hi - opt - l, tbsp, e, n), now);
            n2k_set_calc(ctx, N2K_LAY_PORT_GND_MIN, ground_course(lo + opt + l, tbsp, e, n), now);
            n2k_set_calc(ctx, N2K_LAY_PORT_GND_MAX, ground_course(hi + opt + l, tbsp, e, n), now);
        }
    }
}

static void derive_tws(n2k_ctx *ctx, uint64_t now)
{
    double tws, m, unused;
    if (!fresh(ctx, N2K_TWS, now, &tws))
        return;
    avg_push(&ctx->tws_avg, now, tws, 0);
    if (avg_get(&ctx->tws_avg, now, ctx->avg_s, &m, &unused))
        n2k_set_calc(ctx, N2K_TWS_AVG, m, now);
}

/* --- Dérive par amure --- */

/* Travers sous le vent (unitaire) d'une amure : tribord amures → bâbord. */
static void leeward(const n2k_tack *k, int stbd, double *we, double *wn)
{
    double h = atan2(k->s, k->c) + (stbd ? -PI / 2 : PI / 2);
    *we = sin(h);
    *wn = cos(h);
}

static double clamp_lw(double l)
{
    return l < 0 ? 0 : (l > N2K_LW_MAX ? N2K_LW_MAX : l);
}

/* Met à jour la moyenne de l'amure courante avec un échantillon de courant
 * calculé (e, n) et, si possible, l'estimation de la dérive λ. */
static void tack_update(n2k_ctx *ctx, uint64_t now, double e, double n,
                        double hdg, double bsp, int instr, double ice, double icn)
{
    double twa, rot;
    if (!fresh(ctx, N2K_TWA, now, &twa))
        return;
    double a = fabs(twa);
    int al = a < PI / 2, tk = twa > 0;
    if (al != ctx->tk_allure || tk != ctx->tk_tack) {   /* virement / empannage */
        ctx->tk_allure = al;
        ctx->tk_tack = tk;
        ctx->tk_since = now;
        ctx->tk_prev = 0;
        return;
    }
    if (now - ctx->tk_since < N2K_LW_SETTLE_MS || a < 15 * PI / 180 || a > 165 * PI / 180 ||
        (fresh(ctx, N2K_ROT, now, &rot) && fabs(rot) > N2K_LW_MAX_ROT)) {
        ctx->tk_prev = 0;
        return;
    }

    n2k_tack *k = &ctx->tk[al][tk];
    if (!k->last || now - k->last > N2K_LW_SETTLE_MS) {  /* nouveau bord : on repart */
        k->e = e; k->n = n; k->s = sin(hdg); k->c = cos(hdg); k->b = bsp;
        k->dur = 0;
    } else {
        double dt = ctx->tk_prev && now > ctx->tk_prev ? (double)(now - ctx->tk_prev) / 1000.0 : 0;
        if (dt > 1) dt = 1;
        double al_ = 1 - exp(-dt / (double)ctx->current_s);
        k->e += al_ * (e - k->e);
        k->n += al_ * (n - k->n);
        k->s += al_ * (sin(hdg) - k->s);
        k->c += al_ * (cos(hdg) - k->c);
        k->b += al_ * (bsp - k->b);
        k->dur += dt;
    }
    k->last = now;
    ctx->tk_prev = now;
    if (k->dur < N2K_LW_MIN_DUR || k->b < 0.5)
        return;

    double we, wn;
    leeward(k, tk, &we, &wn);
    if (instr) {
        /* Courant réel connu : la dérive se lit sur l'amure courante. */
        ctx->lw[al] = clamp_lw(((k->e - ice) * we + (k->n - icn) * wn) / k->b);
        ctx->lw_have[al] = 1;
        ctx->lw_t[al] = now;
        return;
    }
    /* Deux amures : c_k = C + λ·B_k·w_k → λ par la différence des deux. */
    const n2k_tack *o = &ctx->tk[al][!tk];
    if (!o->last || o->dur < N2K_LW_MIN_DUR || o->b < 0.5 || now - o->last > N2K_LW_MAX_AGE_MS)
        return;
    double oe, on;
    leeward(o, !tk, &oe, &on);
    double dve = k->b * we - o->b * oe, dvn = k->b * wn - o->b * on;
    double d2 = dve * dve + dvn * dvn;
    if (d2 < 0.01)
        return;                                 /* amures quasi parallèles */
    ctx->lw[al] = clamp_lw(((k->e - o->e) * dve + (k->n - o->n) * dvn) / d2);
    ctx->lw_have[al] = 1;
    ctx->lw_t[al] = o->last;                    /* âge = celui du bord opposé */
}

/* --- Courant calculé --- */

static void derive_current(n2k_ctx *ctx, uint64_t now)
{
    double cog, sog, hdg, bsp, e, n, ice = 0, icn = 0;
    if (!fresh(ctx, N2K_COG, now, &cog) || !fresh(ctx, N2K_SOG, now, &sog) ||
        !fresh(ctx, N2K_HDG_TRUE, now, &hdg) || !fresh(ctx, N2K_BSP, now, &bsp))
        return;
    /* Composantes Est/Nord du vecteur fond moins le vecteur surface. */
    e = sog * sin(cog) - bsp * sin(hdg);
    n = sog * cos(cog) - bsp * cos(hdg);

    int src;
    int instr = n2k_get_info(ctx, N2K_SET, now, &src, NULL, NULL) == N2K_OK &&
                src != N2K_SRC_CALC && current_vec(ctx, now, &ice, &icn);
    tack_update(ctx, now, e, n, hdg, bsp, instr, ice, icn);
    if (instr)
        return;                                 /* l'instrument fournit SET/DRIFT */

    avg_push(&ctx->cur_avg, now, e, n);
    if (!avg_get(&ctx->cur_avg, now, ctx->current_s, &e, &n))
        return;
    double drift = sqrt(e * e + n * n);
    n2k_set_calc(ctx, N2K_DRIFT, drift, now);
    if (drift > 0.01)                           /* direction indéfinie à vitesse nulle */
        n2k_set_calc(ctx, N2K_SET, wrap_2pi(atan2(e, n)), now);
}

int n2k_vmg_profile(const n2k_ctx *ctx, double pct[181])
{
    const n2k_polar *p = ctx->polar;
    double tws = ctx->tws_target, a, v, best[2];
    if (!p || !(tws >= 0))
        return 0;
    for (int up = 0; up < 2; up++) {
        if (!n2k_polar_target(p, tws, up, &a, &v))
            return 0;
        best[up] = v * cos(a) * (up ? 1 : -1);
    }
    for (int k = 0; k <= 180; k++) {
        int up = k < 90;
        double t = k * PI / 180.0;
        double vmg = n2k_polar_speed(p, t, tws) * cos(t) * (up ? 1 : -1);
        double x = 100.0 * vmg / best[up];
        pct[k] = x < 0 ? 0 : (x > 100 ? 100 : x);
    }
    return 1;
}

void n2k_derive(n2k_ctx *ctx, n2k_item item, uint64_t now)
{
    switch (item) {
    /* 130306 écrit l'angle puis la vitesse : on calcule sur la vitesse, quand
     * les deux valeurs du même message sont en place. */
    case N2K_TWS: derive_tws(ctx, now); derive_wind(ctx, now); break;
    case N2K_SOG: derive_current(ctx, now); break;
    default: break;
    }
}
