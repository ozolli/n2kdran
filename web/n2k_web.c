/*
 * n2k_web.c — fine couche entre libn2k (compilée en WebAssembly par emscripten)
 * et la page web (n2kdran.js). Équivalent de app/src/main/cpp/n2k_jni.c.
 *
 * Un seul contexte, une seule page : pas de verrou (JavaScript est mono-thread).
 * Les échanges passent par des tampons statiques dont la page lit l'adresse :
 * entrée (lignes YDRAW, texte de polaire), état (valeurs, statuts), AIS.
 * Les heures sont des doubles (ms, horloge monotone de la page) : pas de BigInt.
 */
#include <emscripten/emscripten.h>
#include <stdlib.h>
#include <string.h>

#include "n2k.h"

#define IN_SIZE   65536
#define AIS_SKIP  64

static n2k_ctx   *ctx;
static n2k_polar *polar;
static char       in_buf[IN_SIZE + 1];
static double     values[N2K_ITEM_COUNT];
static uint8_t    status[N2K_ITEM_COUNT];
static double     profile[181];
static char       err[160];
static char       wp[64];
static uint32_t   skip[AIS_SKIP];
static double     ais_v[7];          /* niveau, MMSI, distance, relèvement, CPA, TCPA, est */
static char       ais_name[N2K_AIS_NAME];

EMSCRIPTEN_KEEPALIVE int    w_init(void)        { if (!ctx) ctx = n2k_create(); return ctx != NULL; }
EMSCRIPTEN_KEEPALIVE int    w_item_count(void)  { return N2K_ITEM_COUNT; }
EMSCRIPTEN_KEEPALIVE const char *w_item_name(int i) { return n2k_item_name((n2k_item)i); }
EMSCRIPTEN_KEEPALIVE char  *w_in(void)          { return in_buf; }
EMSCRIPTEN_KEEPALIVE int    w_in_size(void)     { return IN_SIZE; }
EMSCRIPTEN_KEEPALIVE double *w_values(void)     { return values; }
EMSCRIPTEN_KEEPALIVE uint8_t *w_status(void)    { return status; }
EMSCRIPTEN_KEEPALIVE double *w_profile(void)    { return profile; }
EMSCRIPTEN_KEEPALIVE uint32_t *w_skip(void)     { return skip; }
EMSCRIPTEN_KEEPALIVE double *w_ais_v(void)      { return ais_v; }
EMSCRIPTEN_KEEPALIVE char  *w_ais_name(void)    { return ais_name; }

/* Pousse les `len` octets de in_buf (une ou plusieurs lignes séparées par LF).
 * Retourne le nombre de lignes valides. */
EMSCRIPTEN_KEEPALIVE int w_feed(int len, double now, unsigned flags)
{
    if (len < 0) len = 0;
    if (len > IN_SIZE) len = IN_SIZE;
    in_buf[len] = '\0';
    int ok = 0;
    char *p = in_buf;
    while (*p) {
        char *e = strchr(p, '\n');
        if (e) *e = '\0';
        if (*p && *p != '\r')
            ok += n2k_feed_line_ex(ctx, p, (uint64_t)now, flags);
        if (!e) break;
        p = e + 1;
    }
    return ok;
}

EMSCRIPTEN_KEEPALIVE void w_snapshot(double now) { n2k_snapshot(ctx, (uint64_t)now, values, status); }
EMSCRIPTEN_KEEPALIVE void w_set_stale(int ms)    { n2k_set_stale_ms(ctx, (uint32_t)ms); }
EMSCRIPTEN_KEEPALIVE int  w_vmg_profile(void)    { return n2k_vmg_profile(ctx, profile); }
EMSCRIPTEN_KEEPALIVE void w_ais_reset(void)      { n2k_ais_reset(ctx); }
EMSCRIPTEN_KEEPALIVE void w_ais_still(int on)    { n2k_set_ais_ignore_still(ctx, on); }

/* Polaire : texte dans in_buf (`len` octets). Retourne NULL si OK, sinon le message. */
EMSCRIPTEN_KEEPALIVE const char *w_set_polar(int len)
{
    err[0] = '\0';
    if (len < 0 || len > IN_SIZE) return "polaire trop grande";
    n2k_polar *p = n2k_polar_parse(in_buf, (size_t)len, err, sizeof err);
    if (!p) return err;
    n2k_set_polar(ctx, p);
    n2k_polar_free(polar);
    polar = p;
    return NULL;
}

/* Nom du WP s'il est frais, sinon NULL. */
EMSCRIPTEN_KEEPALIVE const char *w_wp_name(double now)
{
    n2k_status st = n2k_wp_name(ctx, (uint64_t)now, wp, sizeof wp);
    return st == N2K_OK && wp[0] ? wp : NULL;
}

/* Statistiques : lignes, rejetées, fast-packets complets, abandonnés (dans ais_v, réutilisé). */
EMSCRIPTEN_KEEPALIVE double *w_stats(void)
{
    n2k_stats s;
    n2k_get_stats(ctx, &s);
    static double st[4];
    st[0] = (double)s.lines; st[1] = (double)s.bad_lines;
    st[2] = (double)s.fp_complete; st[3] = (double)s.fp_dropped;
    return st;
}

static int ais_out(int level, const n2k_ais_target *t)
{
    if (!level) return 0;
    ais_v[0] = level; ais_v[1] = t->mmsi; ais_v[2] = t->range; ais_v[3] = t->bearing;
    ais_v[4] = t->cpa; ais_v[5] = t->tcpa; ais_v[6] = t->est;
    memcpy(ais_name, t->name, sizeof ais_name);
    ais_name[N2K_AIS_NAME - 1] = '\0';
    return level;
}

/* Cible la plus menaçante (MMSI à sauter : les nskip premiers de skip[]). */
EMSCRIPTEN_KEEPALIVE int w_ais_threat(double now, double cpa, double tcpa, double range, int nskip)
{
    n2k_ais_target t;
    if (nskip > AIS_SKIP) nskip = AIS_SKIP;
    return ais_out(n2k_ais_threat(ctx, (uint64_t)now, cpa, tcpa, range, skip, nskip, &t), &t);
}

EMSCRIPTEN_KEEPALIVE int w_ais_distress(double now, int nskip)
{
    n2k_ais_target t;
    if (nskip > AIS_SKIP) nskip = AIS_SKIP;
    return ais_out(n2k_ais_distress(ctx, (uint64_t)now, skip, nskip, &t), &t);
}
