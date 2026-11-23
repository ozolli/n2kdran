/*
 * n2k.c — Contexte, réassemblage fast-packet, état courant et péremption.
 */
#include "n2k_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char *const item_names[N2K_ITEM_COUNT] = {
    "LAT", "LON", "COG", "SOG", "HDG_TRUE", "HDG_MAG", "VARIATION", "ROT",
    "PITCH", "ROLL", "RUDDER", "BSP", "DEPTH", "DEPTH_OFFSET", "LOG", "TRIP",
    "AWA", "AWS", "TWA", "TWS", "GWA", "GWS", "GWD", "SET", "DRIFT",
    "SEA_TEMP", "AIR_TEMP", "PRESSURE", "HUMIDITY", "GNSS_METHOD", "GNSS_SATS",
    "HDOP", "PDOP", "XTE", "DTW", "BTW", "WCV",
    "TWD", "TWS_AVG", "TWD_AVG", "VMG", "POLAR_BSP", "POLAR_PCT", "TARGET_TWA",
    "TARGET_BSP", "VMG_PCT", "VMG_RANGE_MIN", "VMG_RANGE_MAX", "TWA_VMG_PCT",
    "TWD_MIN", "TWD_MAX", "LAY_STBD", "LAY_PORT", "LAY_STBD_MIN", "LAY_STBD_MAX",
    "LAY_PORT_MIN", "LAY_PORT_MAX", "LAY_STBD_GND", "LAY_PORT_GND",
    "LAY_STBD_GND_MIN", "LAY_STBD_GND_MAX", "LAY_PORT_GND_MIN", "LAY_PORT_GND_MAX",
    "LEEWAY", "LEEWAY_SIGMA", "LAY_STBD_GND_LO", "LAY_STBD_GND_HI",
    "LAY_PORT_GND_LO", "LAY_PORT_GND_HI", "TTW_STBD", "TTW_PORT", "TTW", "DTW_SAIL",
    "OPP_TWA_GND",
};

_Static_assert(sizeof item_names / sizeof item_names[0] == N2K_ITEM_COUNT,
               "item_names désynchronisé de n2k_item");

n2k_ctx *n2k_create(void)
{
    n2k_ctx *c = calloc(1, sizeof *c);
    if (c) {
        c->stale_ms = 3000;
        c->avg_s = 600;
        c->target_s = 60;
        c->current_s = 60;
        c->tk_allure = c->tk_tack = -1;
        c->tws_target = -1;
        c->wp_dest = -1;
        const double deg = 3.14159265358979323846 / 180.0;
        c->lw_prior[1] = 4 * deg;  c->lw_prior_sigma[1] = 4 * deg;   /* près */
        c->lw_prior[0] = 2 * deg;  c->lw_prior_sigma[0] = 2 * deg;   /* portant */
    }
    return c;
}

void n2k_destroy(n2k_ctx *ctx)
{
    free(ctx);
}

void n2k_set_stale_ms(n2k_ctx *ctx, uint32_t ms)
{
    ctx->stale_ms = ms;
}

void n2k_set_ais_ignore_still(n2k_ctx *ctx, int on)
{
    ctx->ais_ignore_still = on != 0;
}

void n2k_set_polar(n2k_ctx *ctx, const n2k_polar *polar)
{
    ctx->polar = polar;
    memset(&ctx->tgt_up, 0, sizeof ctx->tgt_up);
    memset(&ctx->tgt_dn, 0, sizeof ctx->tgt_dn);
}

void n2k_set_leeway_prior(n2k_ctx *ctx, double up, double up_sigma,
                          double down, double down_sigma)
{
    ctx->lw_prior[1] = up;
    ctx->lw_prior_sigma[1] = up_sigma;
    ctx->lw_prior[0] = down;
    ctx->lw_prior_sigma[0] = down_sigma;
}

static uint32_t clamp_window(uint32_t s, uint32_t cur)
{
    if (s == 0) return cur;
    return s > N2K_AVG_SECONDS ? N2K_AVG_SECONDS : s;
}

void n2k_set_windows(n2k_ctx *ctx, uint32_t avg_s, uint32_t target_s, uint32_t current_s)
{
    ctx->avg_s = clamp_window(avg_s, ctx->avg_s);
    ctx->target_s = clamp_window(target_s, ctx->target_s);
    ctx->current_s = clamp_window(current_s, ctx->current_s);
}

const char *n2k_item_name(n2k_item item)
{
    return ((unsigned)item < N2K_ITEM_COUNT) ? item_names[item] : "?";
}

/* Navigation (qtVlm, 1 Hz, diffusée en UDP sans retransmission) : un message
 * multi-trames est perdu dès qu'une trame manque ; ces données variant
 * lentement, leur délai de péremption est porté à N2K_NAV_STALE_MS. */
#define N2K_NAV_STALE_MS 10000u

static uint32_t stale_limit(const n2k_ctx *ctx, int pgn)
{
    if ((pgn == 129283 || pgn == 129284 || pgn == 129285) && ctx->stale_ms < N2K_NAV_STALE_MS)
        return N2K_NAV_STALE_MS;
    return ctx->stale_ms;
}

n2k_status n2k_wp_name(const n2k_ctx *ctx, uint64_t now_ms, char *buf, size_t size)
{
    if (size)
        buf[0] = '\0';
    if (!ctx->wp_name_t)
        return N2K_NEVER;
    if (size) {
        strncpy(buf, ctx->wp_name, size - 1);
        buf[size - 1] = '\0';
        /* Troncature : ne pas laisser de séquence UTF-8 coupée en fin de tampon. */
        size_t n = strlen(buf), k = n;
        while (k > 0 && ((unsigned char)buf[k - 1] & 0xC0) == 0x80)
            k--;                                     /* début de la dernière séquence */
        if (k > 0 && (unsigned char)buf[k - 1] >= 0xC0) {
            unsigned char b = (unsigned char)buf[k - 1];
            size_t need = b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : 2;
            if (n - (k - 1) < need)
                buf[k - 1] = '\0';
        }
    }
    return (now_ms > ctx->wp_name_t && now_ms - ctx->wp_name_t > stale_limit(ctx, 129285)) ? N2K_STALE : N2K_OK;
}

void n2k_get_stats(const n2k_ctx *ctx, n2k_stats *out)
{
    *out = ctx->st;
}

/* --- État --- */

static int is_stale(const n2k_ctx *ctx, const n2k_item_slot *s, uint64_t now)
{
    /* now < t (horloge appelante incohérente) : considéré frais. */
    return now > s->t && now - s->t > stale_limit(ctx, s->pgn);
}

/* Règle d'écriture : la source/PGN qui possède une donnée la garde tant
 * qu'elle est fraîche ; une donnée calculée ne prend jamais la place d'une
 * donnée d'instrument fraîche, et cède toujours la place à un instrument. */
static int store(n2k_ctx *ctx, n2k_item item, double v, int src, int pgn, uint64_t now)
{
    if ((unsigned)item >= N2K_ITEM_COUNT || !isfinite(v))
        return 0;
    n2k_item_slot *s = &ctx->items[item];
    if (s->set && !(s->src == src && s->pgn == pgn) && !is_stale(ctx, s, now) &&
        !(s->src == N2K_SRC_CALC && src != N2K_SRC_CALC))
        return 0;
    s->set = 1;
    s->v = v;
    s->t = now;
    s->src = src;
    s->pgn = pgn;
    return 1;
}

void n2k_set(n2k_ctx *ctx, n2k_item item, double v, int src, int pgn, uint64_t now)
{
    if (store(ctx, item, v, src, pgn, now))
        n2k_derive(ctx, item, now);
}

void n2k_set_calc(n2k_ctx *ctx, n2k_item item, double v, uint64_t now)
{
    store(ctx, item, v, N2K_SRC_CALC, 0, now);
}

n2k_status n2k_get_info(const n2k_ctx *ctx, n2k_item item, uint64_t now_ms,
                        int *src, int *pgn, uint64_t *age_ms)
{
    if ((unsigned)item >= N2K_ITEM_COUNT || !ctx->items[item].set)
        return N2K_NEVER;
    const n2k_item_slot *s = &ctx->items[item];
    if (src)    *src = s->src;
    if (pgn)    *pgn = s->pgn;
    if (age_ms) *age_ms = (now_ms > s->t) ? now_ms - s->t : 0;
    return is_stale(ctx, s, now_ms) ? N2K_STALE : N2K_OK;
}

n2k_status n2k_get(const n2k_ctx *ctx, n2k_item item, uint64_t now_ms, double *value)
{
    n2k_status st = n2k_get_info(ctx, item, now_ms, NULL, NULL, NULL);
    if (st != N2K_NEVER && value)
        *value = ctx->items[item].v;
    return st;
}

void n2k_snapshot(const n2k_ctx *ctx, uint64_t now_ms, double *values, uint8_t *status)
{
    for (int i = 0; i < N2K_ITEM_COUNT; i++) {
        double v = NAN;
        status[i] = (uint8_t)n2k_get(ctx, (n2k_item)i, now_ms, &v);
        values[i] = v;
    }
}

/* --- Fast-packet ---
 * Trame 0 : [seq<<5 | 0][longueur totale][6 octets]
 * Trame n : [seq<<5 | n][7 octets]
 * Un message n'est accepté que si ses trames arrivent toutes, dans l'ordre,
 * avec le même compteur de séquence. Sinon il est abandonné. */

static n2k_fp_slot *fp_find(n2k_ctx *ctx, int pgn, int src)
{
    for (int i = 0; i < N2K_FP_SLOTS; i++)
        if (ctx->fp[i].used && ctx->fp[i].pgn == pgn && ctx->fp[i].src == src)
            return &ctx->fp[i];
    return NULL;
}

static n2k_fp_slot *fp_alloc(n2k_ctx *ctx)
{
    n2k_fp_slot *oldest = &ctx->fp[0];
    for (int i = 0; i < N2K_FP_SLOTS; i++) {
        if (!ctx->fp[i].used)
            return &ctx->fp[i];
        if (ctx->fp[i].t0 < oldest->t0)
            oldest = &ctx->fp[i];
    }
    ctx->st.fp_dropped++;           /* table pleine : on sacrifie le plus ancien */
    return oldest;
}

static void fp_drop(n2k_ctx *ctx, n2k_fp_slot *s)
{
    s->used = 0;
    ctx->st.fp_dropped++;
}

static void decode(n2k_ctx *ctx, const n2k_pgn_def *def, int src,
                   const uint8_t *d, int len, uint64_t now)
{
    if (len < def->minlen) {
        ctx->st.decode_errors++;
        return;
    }
    def->fn(ctx, src, def->pgn, d, len, now);
}

static void fp_frame(n2k_ctx *ctx, const n2k_pgn_def *def, int src,
                     const uint8_t *d, int len, uint64_t now)
{
    if (len < 1)
        return;
    int seq = d[0] >> 5;
    int idx = d[0] & 0x1F;
    n2k_fp_slot *s = fp_find(ctx, def->pgn, src);

    /* Trame reçue en double (diffusion sur plusieurs interfaces, renvoi) :
     * même séquence, index de la trame précédente → ignorée sans casser le message. */
    if (s && s->seq == seq && idx == s->next - 1)
        return;

    if (idx == 0) {
        if (s)
            fp_drop(ctx, s);        /* message précédent resté incomplet */
        if (len < 2 || d[1] == 0 || d[1] > N2K_FP_MAX)
            return;
        s = fp_alloc(ctx);
        s->used = 1;
        s->pgn = def->pgn;
        s->src = src;
        s->seq = seq;
        s->total = d[1];
        s->got = 0;
        s->next = 1;
        s->t0 = now;
        int n = len - 2;
        if (n > 6) n = 6;
        if (n > s->total) n = s->total;
        memcpy(s->buf, d + 2, (size_t)n);
        s->got = n;
    } else {
        if (!s)
            return;                 /* trame orpheline (début de flux, perte) */
        if (s->seq != seq || s->next != idx ||
            (now > s->t0 && now - s->t0 > N2K_FP_TIMEOUT)) {
            fp_drop(ctx, s);
            return;
        }
        int n = len - 1;
        if (n > 7) n = 7;
        if (n > s->total - s->got) n = s->total - s->got;
        memcpy(s->buf + s->got, d + 1, (size_t)n);
        s->got += n;
        s->next++;
        if (n < 7 && s->got < s->total) {   /* trame courte au milieu */
            fp_drop(ctx, s);
            return;
        }
    }

    if (s->got >= s->total) {
        s->used = 0;
        ctx->st.fp_complete++;
        decode(ctx, def, src, s->buf, s->total, now);
    }
}

void n2k_feed_frame(n2k_ctx *ctx, uint32_t can_id, const uint8_t *data, int len,
                    uint64_t now_ms)
{
    int pgn, src;
    n2k_decode_id(can_id, NULL, &pgn, &src, NULL);
    const n2k_pgn_def *def = n2k_find_pgn(pgn);
    if (!def) {
        ctx->st.frames_ignored++;   /* filtré avant tout réassemblage */
        return;
    }
    ctx->st.frames_used++;
    if (def->fast)
        fp_frame(ctx, def, src, data, len, now_ms);
    else
        decode(ctx, def, src, data, len, now_ms);
}

int n2k_feed_line_ex(n2k_ctx *ctx, const char *line, uint64_t now_ms, unsigned flags)
{
    uint32_t id;
    uint8_t d[8];
    int len, pgn;

    ctx->st.lines++;
    if (!n2k_parse_ydraw(line, NULL, &id, d, &len)) {
        ctx->st.bad_lines++;
        return 0;
    }
    if (flags & N2K_FEED_NAV_ONLY) {
        n2k_decode_id(id, NULL, &pgn, NULL, NULL);
        if (pgn != 129283 && pgn != 129284 && pgn != 129285) {
            ctx->st.frames_ignored++;
            return 1;
        }
    }
    n2k_feed_frame(ctx, id, d, len, now_ms);
    return 1;
}

int n2k_feed_line(n2k_ctx *ctx, const char *line, uint64_t now_ms)
{
    return n2k_feed_line_ex(ctx, line, now_ms, 0);
}
