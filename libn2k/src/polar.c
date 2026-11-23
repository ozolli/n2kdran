/*
 * polar.c — Polaire de vitesse au format texte qtVlm, interpolation et cibles.
 *
 * Format constaté sur les fichiers de qtVlm (pas de spécification officielle) :
 *   TWA\TWS;0;4;6;...      ← TWS en nœuds
 *   0;0.00;0.00;...        ← TWA en degrés, puis BSP en nœuds
 *   5;0.00;0.40;...
 * Séparateur `;` ou tabulation, LF ou CRLF, lignes `!` = commentaires (comme polar_doctor). Analyse des nombres faite à la main
 * (strtod dépend de la locale, le point décimal n'est pas garanti).
 */
#include "n2k.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TWS 64
#define MAX_TWA 361
#define PI      3.14159265358979323846
#define KN      (1852.0 / 3600.0)      /* m/s par nœud */

struct n2k_polar {
    int    ntws, ntwa;
    int    tws_cap;                    /* index de la dernière colonne non nulle */
    double tws[MAX_TWS];               /* nœuds, croissant */
    double twa[MAX_TWA];               /* degrés, croissant, dans [0, 180] */
    double bsp[MAX_TWA][MAX_TWS];      /* nœuds */
};

static void set_err(char *err, size_t sz, int line, const char *msg)
{
    if (err && sz)
        snprintf(err, sz, "ligne %d : %s", line, msg);
}

/* Nombre décimal simple : [-]chiffres[.chiffres]. Avance *p. */
static int parse_num(const char **p, const char *end, double *out)
{
    const char *s = *p;
    int neg = 0, nd = 0;
    double v = 0;
    while (s < end && *s == ' ') s++;
    if (s < end && (*s == '-' || *s == '+')) neg = (*s++ == '-');
    while (s < end && *s >= '0' && *s <= '9') { v = v * 10 + (*s++ - '0'); nd++; }
    if (s < end && *s == '.') {
        double f = 0.1;
        s++;
        while (s < end && *s >= '0' && *s <= '9') { v += (*s++ - '0') * f; f *= 0.1; nd++; }
    }
    while (s < end && *s == ' ') s++;
    if (nd == 0)
        return 0;
    *out = neg ? -v : v;
    *p = s;
    return 1;
}

static int is_sep(char c) { return c == ';' || c == '\t'; }

/* Découpe une ligne [s, e) en nombres ; `skip_first` ignore la 1re cellule
 * (en-tête `TWA\TWS`). Retourne le nombre de valeurs, -1 si cellule invalide. */
static int parse_row(const char *s, const char *e, int skip_first, double *out, int max)
{
    int n = 0;
    if (skip_first) {
        while (s < e && !is_sep(*s)) s++;
        if (s == e)
            return 0;
        s++;
    }
    for (;;) {
        if (n >= max)
            return -1;
        if (!parse_num(&s, e, &out[n]))
            return -1;
        n++;
        if (s == e)
            return n;
        if (!is_sep(*s))
            return -1;
        s++;
        if (s == e)                     /* séparateur final toléré */
            return n;
    }
}

n2k_polar *n2k_polar_parse(const char *text, size_t len, char *err, size_t errsz)
{
    n2k_polar *p = calloc(1, sizeof *p);
    if (!p) {
        set_err(err, errsz, 0, "mémoire insuffisante");
        return NULL;
    }
    const char *s = text, *end = text + len;
    int line = 0, header = 1;

    while (s < end) {
        const char *e = memchr(s, '\n', (size_t)(end - s));
        const char *next = e ? e + 1 : end;
        if (!e) e = end;
        while (e > s && (e[-1] == '\r' || e[-1] == ' ')) e--;
        line++;
        if (e == s || *s == '!') { s = next; continue; }   /* vide ou commentaire */

        if (header) {
            int n = parse_row(s, e, 1, p->tws, MAX_TWS);
            if (n < 1) { set_err(err, errsz, line, "en-tête TWS invalide"); goto fail; }
            for (int i = 1; i < n; i++)
                if (!(p->tws[i] > p->tws[i - 1])) {
                    set_err(err, errsz, line, "TWS non croissantes"); goto fail;
                }
            if (p->tws[0] < 0) { set_err(err, errsz, line, "TWS négative"); goto fail; }
            p->ntws = n;
            header = 0;
        } else {
            double row[MAX_TWS + 1];
            int n = parse_row(s, e, 0, row, MAX_TWS + 1);
            if (n != p->ntws + 1) {
                set_err(err, errsz, line, "nombre de colonnes incohérent"); goto fail;
            }
            if (p->ntwa >= MAX_TWA) { set_err(err, errsz, line, "trop de lignes"); goto fail; }
            double a = row[0];
            if (a < 0 || a > 180 || (p->ntwa > 0 && !(a > p->twa[p->ntwa - 1]))) {
                set_err(err, errsz, line, "TWA hors [0,180] ou non croissant"); goto fail;
            }
            p->twa[p->ntwa] = a;
            for (int i = 0; i < p->ntws; i++) {
                if (row[i + 1] < 0) { set_err(err, errsz, line, "BSP négative"); goto fail; }
                p->bsp[p->ntwa][i] = row[i + 1];
            }
            p->ntwa++;
        }
        s = next;
    }
    if (p->ntwa < 1) {
        set_err(err, errsz, line, "aucune ligne de données");
        goto fail;
    }
    p->tws_cap = -1;
    for (int j = 0; j < p->ntws; j++)
        for (int i = 0; i < p->ntwa; i++)
            if (p->bsp[i][j] > 0) { p->tws_cap = j; break; }
    if (p->tws_cap < 0) {
        set_err(err, errsz, line, "polaire entièrement nulle");
        goto fail;
    }
    return p;
fail:
    free(p);
    return NULL;
}

void n2k_polar_free(n2k_polar *p)
{
    free(p);
}

/* Localise x dans axe[0..n-1] avec un point implicite (0, valeur 0) si axe[0] > 0.
 * Retourne i0, i1 (-1 = point implicite) et le poids t de i1. */
static void locate(const double *axe, int n, double x, int *i0, int *i1, double *t)
{
    if (x <= axe[0]) {
        if (axe[0] <= 0) {                  /* premier point = 0 : borne basse */
            *i0 = *i1 = 0; *t = 0;
        } else if (x <= 0) {                /* sur le point implicite */
            *i0 = *i1 = -1; *t = 0;
        } else {                            /* entre le point implicite et axe[0] */
            *i0 = -1; *i1 = 0; *t = x / axe[0];
        }
        return;
    }
    if (x >= axe[n - 1]) { *i0 = *i1 = n - 1; *t = 0; return; }
    int k = 0;
    while (axe[k + 1] < x) k++;
    *i0 = k; *i1 = k + 1;
    *t = (x - axe[k]) / (axe[k + 1] - axe[k]);
}

static double cell(const n2k_polar *p, int ia, int is)
{
    return (ia < 0 || is < 0) ? 0.0 : p->bsp[ia][is];
}

/* BSP en nœuds pour TWA en degrés [0,180] et TWS en nœuds. */
static double speed_kn(const n2k_polar *p, double a, double w)
{
    if (w > p->tws[p->tws_cap])
        w = p->tws[p->tws_cap];
    int a0, a1, s0, s1;
    double ta, ts;
    locate(p->twa, p->ntwa, a, &a0, &a1, &ta);
    locate(p->tws, p->tws_cap + 1, w, &s0, &s1, &ts);
    double v0 = cell(p, a0, s0) * (1 - ts) + cell(p, a0, s1) * ts;
    double v1 = cell(p, a1, s0) * (1 - ts) + cell(p, a1, s1) * ts;
    return v0 * (1 - ta) + v1 * ta;
}

static double norm_twa_deg(double twa)
{
    double a = fabs(fmod(twa, 2 * PI)) * 180.0 / PI;   /* [0, 360) */
    return a > 180 ? 360 - a : a;
}

double n2k_polar_speed(const n2k_polar *p, double twa, double tws)
{
    if (!p || !isfinite(twa) || !isfinite(tws) || tws < 0)
        return NAN;
    return speed_kn(p, norm_twa_deg(twa), tws / KN) * KN;
}

int n2k_polar_target(const n2k_polar *p, double tws, int upwind,
                     double *twa_opt, double *bsp_target)
{
    if (!p || !isfinite(tws) || tws < 0)
        return 0;
    double w = tws / KN, best = 0, best_a = 0, best_v = 0;
    for (int k = 0; k <= 900; k++) {
        double a = upwind ? k * 0.1 : 90 + k * 0.1;
        double v = speed_kn(p, a, w);
        double vmg = v * cos(a * PI / 180.0) * (upwind ? 1 : -1);
        if (vmg > best) { best = vmg; best_a = a; best_v = v; }
    }
    if (best <= 0)
        return 0;
    if (twa_opt)    *twa_opt = best_a * PI / 180.0;
    if (bsp_target) *bsp_target = best_v * KN;
    return 1;
}

/* VMG (nœuds, positif dans le sens de l'allure) pour TWA en degrés et TWS en nœuds. */
static double vmg_kn(const n2k_polar *p, double a, double w, int upwind)
{
    return speed_kn(p, a, w) * cos(a * PI / 180.0) * (upwind ? 1 : -1);
}

int n2k_polar_vmg_range(const n2k_polar *p, double tws, int upwind, double frac,
                        double *twa_min, double *twa_max)
{
    double a_opt, v_opt;
    if (!n2k_polar_target(p, tws, upwind, &a_opt, &v_opt) || !(frac > 0) || frac > 1)
        return 0;
    double w = tws / KN;
    double best = v_opt / KN * cos(a_opt) * (upwind ? 1 : -1);
    double lo = 999, hi = -1;
    for (int k = 0; k <= 900; k++) {
        double a = upwind ? k * 0.1 : 90 + k * 0.1;
        if (vmg_kn(p, a, w, upwind) >= frac * best) {
            if (a < lo) lo = a;
            if (a > hi) hi = a;
        }
    }
    if (twa_min) *twa_min = lo * PI / 180.0;
    if (twa_max) *twa_max = hi * PI / 180.0;
    return 1;
}

int n2k_polar_target_stable(const n2k_polar *p, double tws, int upwind, double hyst,
                            n2k_target_state *st, double *twa_opt, double *bsp_target)
{
    double a_glob, v_glob;
    if (!st || !n2k_polar_target(p, tws, upwind, &a_glob, &v_glob))
        return 0;
    double w = tws / KN;
    double g_deg = a_glob * 180.0 / PI;
    double chosen = g_deg;

    if (st->valid) {
        /* Montée de gradient depuis l'angle précédent : sommet local du pic
         * sur lequel on se trouvait, sans sauter dans la vallée voisine. */
        double lo = upwind ? 0 : 90, hi = upwind ? 90 : 180;
        double l_deg = st->twa * 180.0 / PI;
        if (l_deg < lo) l_deg = lo;
        if (l_deg > hi) l_deg = hi;
        double l_vmg = vmg_kn(p, l_deg, w, upwind);
        for (int guard = 0; guard < 1800; guard++) {
            double up = l_deg + 0.1 <= hi ? vmg_kn(p, l_deg + 0.1, w, upwind) : -1e9;
            double dn = l_deg - 0.1 >= lo ? vmg_kn(p, l_deg - 0.1, w, upwind) : -1e9;
            if (up > l_vmg && up >= dn)      { l_deg += 0.1; l_vmg = up; }
            else if (dn > l_vmg)             { l_deg -= 0.1; l_vmg = dn; }
            else break;
        }
        double g_vmg = vmg_kn(p, g_deg, w, upwind);
        if (l_vmg > 0 && g_vmg <= l_vmg * (1 + hyst))
            chosen = l_deg;                 /* pas de gain net : on reste sur ce pic */
    }
    st->valid = 1;
    st->twa = chosen * PI / 180.0;
    if (twa_opt)    *twa_opt = st->twa;
    if (bsp_target) *bsp_target = speed_kn(p, chosen, w) * KN;
    return 1;
}
