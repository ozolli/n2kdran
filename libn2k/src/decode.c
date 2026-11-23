/*
 * decode.c — Décodeurs des PGN utilisés par n2kdran.
 *
 * Positions, tailles, résolutions et signes des champs : canboat docs/canboat.json.
 * Valeurs indisponibles (canboat fieldtype.c, d'après Cassidy) : les valeurs
 * du haut de la plage sont réservées — 3 pour un champ de 8 bits et plus,
 * 2 pour 4 à 7 bits, 1 pour 2 ou 3 bits. Elles ne sont jamais écrites dans l'état.
 */
#include "n2k_internal.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#define N2K_PI 3.14159265358979323846
#define TWO_PI (2.0 * N2K_PI)

static unsigned u16(const uint8_t *d) { return (unsigned)d[0] | (unsigned)d[1] << 8; }
static uint32_t u24(const uint8_t *d) { return (uint32_t)d[0] | (uint32_t)d[1] << 8 | (uint32_t)d[2] << 16; }
static uint32_t u32(const uint8_t *d) { return u24(d) | (uint32_t)d[3] << 24; }
static int32_t  s16(const uint8_t *d) { return (int16_t)u16(d); }
static int32_t  s32(const uint8_t *d) { return (int32_t)u32(d); }

/* Disponibilité selon les règles N2K. */
static int ok_u8(unsigned v)  { return v <= 0xFCu; }
static int ok_u16(unsigned v) { return v <= 0xFFFCu; }
static int ok_u24(uint32_t v) { return v <= 0xFFFFFCu; }
static int ok_u32(uint32_t v) { return v <= 0xFFFFFFFCu; }
static int ok_s16(int32_t v)  { return v <= 0x7FFC; }
static int ok_s32(int32_t v)  { return v <= 0x7FFFFFFC; }

/* Angle relatif au bateau ramené dans (-π, π]. */
static double rel_angle(double a)
{
    a = fmod(a, TWO_PI);
    if (a > N2K_PI)   a -= TWO_PI;
    if (a <= -N2K_PI) a += TWO_PI;
    return a;
}

#define SET(item, v) n2k_set(ctx, (item), (v), src, pgn, now)

/* 127245 Rudder : position s16 @4, 1e-4 rad. */
static void d127245(n2k_ctx *ctx, int src, int pgn, const uint8_t *d, int len, uint64_t now)
{
    (void)len;
    int32_t pos = s16(d + 4);
    if (ok_s16(pos)) SET(N2K_RUDDER, pos * 1e-4);
}

/* 127250 Vessel Heading : heading u16 @1, deviation s16 @3, variation s16 @5
 * (1e-4 rad), reference 2 bits @7 (0 vrai, 1 magnétique). */
static void d127250(n2k_ctx *ctx, int src, int pgn, const uint8_t *d, int len, uint64_t now)
{
    (void)len;
    unsigned hdg = u16(d + 1);
    int32_t var = s16(d + 5);
    unsigned ref = d[7] & 0x3u;
    if (ok_u16(hdg)) {
        if (ref == 0) SET(N2K_HDG_TRUE, hdg * 1e-4);
        else if (ref == 1) SET(N2K_HDG_MAG, hdg * 1e-4);
    }
    if (ok_s16(var)) SET(N2K_VARIATION, var * 1e-4);
}

/* 127251 Rate of Turn : s32 @1, 3.125e-8 rad/s. */
static void d127251(n2k_ctx *ctx, int src, int pgn, const uint8_t *d, int len, uint64_t now)
{
    (void)len;
    int32_t r = s32(d + 1);
    if (ok_s32(r)) SET(N2K_ROT, r * 3.125e-8);
}

/* 127257 Attitude : yaw s16 @1, pitch s16 @3, roll s16 @5, 1e-4 rad. */
static void d127257(n2k_ctx *ctx, int src, int pgn, const uint8_t *d, int len, uint64_t now)
{
    (void)len;
    int32_t pitch = s16(d + 3), roll = s16(d + 5);
    if (ok_s16(pitch)) SET(N2K_PITCH, pitch * 1e-4);
    if (ok_s16(roll))  SET(N2K_ROLL, roll * 1e-4);
}

/* 127258 Magnetic Variation : variation s16 @4, 1e-4 rad. */
static void d127258(n2k_ctx *ctx, int src, int pgn, const uint8_t *d, int len, uint64_t now)
{
    (void)len;
    int32_t var = s16(d + 4);
    if (ok_s16(var)) SET(N2K_VARIATION, var * 1e-4);
}

/* 128259 Speed : speedWaterReferenced u16 @1, 0.01 m/s. */
static void d128259(n2k_ctx *ctx, int src, int pgn, const uint8_t *d, int len, uint64_t now)
{
    (void)len;
    unsigned v = u16(d + 1);
    if (ok_u16(v)) SET(N2K_BSP, v * 0.01);
}

/* 128267 Water Depth : depth u32 @1 (0.01 m), offset s16 @5 (0.001 m). */
static void d128267(n2k_ctx *ctx, int src, int pgn, const uint8_t *d, int len, uint64_t now)
{
    (void)len;
    uint32_t dep = u32(d + 1);
    int32_t off = s16(d + 5);
    if (ok_u32(dep)) SET(N2K_DEPTH, dep * 0.01);
    if (ok_s16(off)) SET(N2K_DEPTH_OFFSET, off * 0.001);
}

/* 128275 Distance Log (fast, 14) : log u32 @6, tripLog u32 @10, 1 m. */
static void d128275(n2k_ctx *ctx, int src, int pgn, const uint8_t *d, int len, uint64_t now)
{
    (void)len;
    uint32_t log = u32(d + 6), trip = u32(d + 10);
    if (ok_u32(log))  SET(N2K_LOG, (double)log);
    if (ok_u32(trip)) SET(N2K_TRIP, (double)trip);
}

/* 129025 Position Rapid Update : lat s32 @0, lon s32 @4, 1e-7 deg. */
static void d129025(n2k_ctx *ctx, int src, int pgn, const uint8_t *d, int len, uint64_t now)
{
    (void)len;
    int32_t lat = s32(d), lon = s32(d + 4);
    if (ok_s32(lat) && ok_s32(lon) && lat >= -900000000 && lat <= 900000000 &&
        lon >= -1800000000 && lon <= 1800000000) {
        SET(N2K_LAT, lat * 1e-7);
        SET(N2K_LON, lon * 1e-7);
    }
}

/* 129026 COG & SOG Rapid Update : ref 2 bits @1, cog u16 @2 (1e-4 rad),
 * sog u16 @4 (0.01 m/s). COG magnétique ignoré. */
static void d129026(n2k_ctx *ctx, int src, int pgn, const uint8_t *d, int len, uint64_t now)
{
    (void)len;
    unsigned ref = d[1] & 0x3u, cog = u16(d + 2), sog = u16(d + 4);
    if (ref == 0 && ok_u16(cog)) SET(N2K_COG, cog * 1e-4);
    if (ok_u16(sog)) SET(N2K_SOG, sog * 0.01);
}

/* 129029 GNSS Position Data (fast, 43+) : method 4 bits haut @31,
 * numberOfSvs u8 @33, hdop s16 @34, pdop s16 @36 (0.01). */
static void d129029(n2k_ctx *ctx, int src, int pgn, const uint8_t *d, int len, uint64_t now)
{
    (void)len;
    unsigned method = d[31] >> 4, svs = d[33];
    int32_t hdop = s16(d + 34), pdop = s16(d + 36);
    if (method <= 13)  SET(N2K_GNSS_METHOD, (double)method);
    if (ok_u8(svs))    SET(N2K_GNSS_SATS, (double)svs);
    if (ok_s16(hdop))  SET(N2K_HDOP, hdop * 0.01);
    if (ok_s16(pdop))  SET(N2K_PDOP, pdop * 0.01);
}

/* 129283 Cross Track Error : xte s32 @2, 0.01 m. */
static void d129283(n2k_ctx *ctx, int src, int pgn, const uint8_t *d, int len, uint64_t now)
{
    (void)len;
    int32_t xte = s32(d + 2);
    if (ok_s32(xte)) SET(N2K_XTE, xte * 0.01);
}

/* 129284 Navigation Data (fast, 34) : distance u32 @1 (0.01 m), ref 2 bits @5,
 * bearing position→WP u16 @14 (1e-4 rad), closing velocity s16 @32 (0.01 m/s). */
static void d129284(n2k_ctx *ctx, int src, int pgn, const uint8_t *d, int len, uint64_t now)
{
    (void)len;
    uint32_t dtw = u32(d + 1);
    unsigned ref = d[5] & 0x3u, btw = u16(d + 14);
    int32_t wcv = s16(d + 32);
    if (ok_u32(dtw)) SET(N2K_DTW, dtw * 0.01);
    if (ref == 0 && ok_u16(btw)) SET(N2K_BTW, btw * 1e-4);
    if (ok_s16(wcv)) SET(N2K_WCV, wcv * 0.01);
    uint32_t dest = u32(d + 20);                 /* destinationWaypointNumber */
    ctx->wp_dest = ok_u32(dest) ? (long)dest : -1;
}

/* Chaîne STRING_LAU (canboat) : [longueur totale][codage][octets] ;
 * codage 1 = ASCII/UTF-8, 0 = UTF-16 LE (réduit ici au Latin-1 → UTF-8).
 * Retourne le nombre d'octets consommés, 0 si invalide. */
/* Longueur de la séquence UTF-8 valide commençant en c (n octets dispo), 0 si invalide. */
static int utf8_seq(const uint8_t *c, int n)
{
    int len = c[0] < 0x80 ? 1 : (c[0] & 0xE0) == 0xC0 ? 2 : (c[0] & 0xF0) == 0xE0 ? 3
            : (c[0] & 0xF8) == 0xF0 ? 4 : 0;
    if (!len || len > n || (len == 2 && c[0] < 0xC2))
        return 0;
    for (int i = 1; i < len; i++)
        if ((c[i] & 0xC0) != 0x80)
            return 0;
    return len;
}

/* Chaîne « ASCII » d'un LAU : en pratique UTF-8 ou Latin-1 selon l'émetteur (qtVlm
 * envoie les accents des noms de WP). Sortie toujours en UTF-8 valide : UTF-8 gardé
 * s'il est valide en entier, sinon octets lus comme Latin-1. Jamais de séquence coupée. */
static void lau_ascii(const uint8_t *c, int n, char *out, size_t size)
{
    int utf8 = 1;
    for (int i = 0; i < n && utf8;) {
        int l = c[i] == 0xFF ? 1 : utf8_seq(c + i, n - i);
        if (!l) utf8 = 0;
        i += l ? l : 1;
    }
    size_t k = 0;
    for (int i = 0; i < n;) {
        if (c[i] < 0x20 || c[i] == 0xFF) { i++; continue; }
        if (utf8) {
            int l = utf8_seq(c + i, n - i);
            if (k + (size_t)l + 1 > size) break;
            memcpy(out + k, c + i, (size_t)l); k += (size_t)l; i += l;
        } else if (c[i] < 0x80) {
            if (k + 2 > size) break;
            out[k++] = (char)c[i++];
        } else {
            if (k + 3 > size) break;
            out[k++] = (char)(0xC0 | (c[i] >> 6)); out[k++] = (char)(0x80 | (c[i] & 0x3F)); i++;
        }
    }
    out[k] = '\0';
}

static int lau_string(const uint8_t *d, int avail, char *out, size_t size)
{
    if (avail < 2 || d[0] < 2 || d[0] > avail)
        return 0;
    int n = d[0] - 2, k = 0;
    const uint8_t *c = d + 2;
    if (d[1] == 1) {
        lau_ascii(c, n, out, size);
        return d[0];
    } else if (d[1] == 0) {
        for (int i = 0; i + 1 < n && (size_t)k + 2 < size; i += 2) {
            unsigned u = c[i] | (unsigned)c[i + 1] << 8;
            if (u < 0x20) continue;
            if (u < 0x80) out[k++] = (char)u;
            else if (u < 0x100) { out[k++] = (char)(0xC0 | (u >> 6)); out[k++] = (char)(0x80 | (u & 0x3F)); }
            else out[k++] = '?';
        }
    } else {
        return 0;
    }
    out[k] = '\0';
    return d[0];
}

/* 129285 Navigation - Route/WP Information (fast) : startRps u16 @0,
 * nItems u16 @2, databaseId @4, routeId @6, bits @8, routeName LAU @9,
 * réservé 1 octet, puis nItems × { wpId u16, wpName LAU, lat s32, lon s32 }.
 * On garde le nom du WP de destination de 129284 (sinon le premier). */
static void d129285(n2k_ctx *ctx, int src, int pgn, const uint8_t *d, int len, uint64_t now)
{
    (void)src; (void)pgn;
    unsigned items = u16(d + 2);
    char tmp[48], first[48] = "", found[48] = "";
    int has_found = 0;
    int o = 9;
    int k = lau_string(d + o, len - o, tmp, sizeof tmp);    /* nom de route (non utilisé) */
    if (!k)
        return;
    o += k + 1;                                              /* + octet réservé */
    for (unsigned i = 0; i < items && i < 64; i++) {
        if (o + 2 > len)
            break;
        unsigned id = u16(d + o);
        o += 2;
        k = lau_string(d + o, len - o, tmp, sizeof tmp);
        if (!k)
            break;
        o += k + 8;                                          /* + latitude, longitude */
        if (i == 0)
            memcpy(first, tmp, sizeof first);
        if (ctx->wp_dest >= 0 && (long)id == ctx->wp_dest) {
            memcpy(found, tmp, sizeof found);
            has_found = 1;
        }
    }
    const char *name = has_found ? found : first;
    if (!name[0])
        return;
    memcpy(ctx->wp_name, name, sizeof ctx->wp_name);
    ctx->wp_name_t = now ? now : 1;
}

/* 129291 Set & Drift Rapid Update : ref 2 bits @1, set u16 @2 (1e-4 rad),
 * drift u16 @4 (0.01 m/s). */
static void d129291(n2k_ctx *ctx, int src, int pgn, const uint8_t *d, int len, uint64_t now)
{
    (void)len;
    unsigned ref = d[1] & 0x3u, set = u16(d + 2), drift = u16(d + 4);
    if (ref == 0 && ok_u16(set)) SET(N2K_SET, set * 1e-4);
    if (ok_u16(drift)) SET(N2K_DRIFT, drift * 0.01);
}

/* 129539 GNSS DOPs : hdop s16 @2, 0.01. */
static void d129539(n2k_ctx *ctx, int src, int pgn, const uint8_t *d, int len, uint64_t now)
{
    (void)len;
    int32_t hdop = s16(d + 2);
    if (ok_s16(hdop)) SET(N2K_HDOP, hdop * 0.01);
}

/* 130306 Wind Data : speed u16 @1 (0.01 m/s), angle u16 @3 (1e-4 rad),
 * reference 3 bits @5 : 0 vrai sol/Nord, 1 magnétique, 2 apparent,
 * 3 vrai sol/bateau, 4 vrai eau. */
static void d130306(n2k_ctx *ctx, int src, int pgn, const uint8_t *d, int len, uint64_t now)
{
    (void)len;
    unsigned spd = u16(d + 1), ang = u16(d + 3), ref = d[5] & 0x7u;
    int sok = ok_u16(spd), aok = ok_u16(ang);
    double s = spd * 0.01, a = ang * 1e-4;
    switch (ref) {
    case 0:
        if (aok) SET(N2K_GWD, a);
        if (sok) SET(N2K_GWS, s);
        break;
    case 2:
        if (aok) SET(N2K_AWA, rel_angle(a));
        if (sok) SET(N2K_AWS, s);
        break;
    case 3:
        if (aok) SET(N2K_GWA, rel_angle(a));
        if (sok) SET(N2K_GWS, s);
        break;
    case 4:
        if (aok) SET(N2K_TWA, rel_angle(a));
        if (sok) SET(N2K_TWS, s);
        break;
    default:
        break;                      /* magnétique : non utilisé */
    }
}

static void set_temp(n2k_ctx *ctx, int src, int pgn, unsigned source, double k, uint64_t now)
{
    if (source == 0) SET(N2K_SEA_TEMP, k);
    else if (source == 1) SET(N2K_AIR_TEMP, k);
}

/* 130311 Environmental Parameters : tempSource 6 bits @1, humSource 2 bits @1>>6,
 * temp u16 @2 (0.01 K), humidity s16 @4 (0.004 %), pressure u16 @6 (100 Pa). */
static void d130311(n2k_ctx *ctx, int src, int pgn, const uint8_t *d, int len, uint64_t now)
{
    (void)len;
    unsigned tsrc = d[1] & 0x3Fu, t = u16(d + 2), p = u16(d + 6);
    int32_t h = s16(d + 4);
    if (tsrc <= 61 && ok_u16(t)) set_temp(ctx, src, pgn, tsrc, t * 0.01, now);
    if (ok_s16(h)) SET(N2K_HUMIDITY, h * 0.004);
    if (ok_u16(p)) SET(N2K_PRESSURE, p * 100.0);
}

/* 130312 Temperature : source u8 @2, actual u16 @3 (0.01 K). */
static void d130312(n2k_ctx *ctx, int src, int pgn, const uint8_t *d, int len, uint64_t now)
{
    (void)len;
    unsigned t = u16(d + 3);
    if (ok_u16(t)) set_temp(ctx, src, pgn, d[2], t * 0.01, now);
}

/* 130314 Actual Pressure : source u8 @2 (0 atmosphérique), pressure s32 @3 (0.1 Pa). */
static void d130314(n2k_ctx *ctx, int src, int pgn, const uint8_t *d, int len, uint64_t now)
{
    (void)len;
    int32_t p = s32(d + 3);
    if (d[2] == 0 && ok_s32(p)) SET(N2K_PRESSURE, p * 0.1);
}

/* 130316 Temperature Extended Range : source u8 @2, temp u24 @3 (0.001 K). */
static void d130316(n2k_ctx *ctx, int src, int pgn, const uint8_t *d, int len, uint64_t now)
{
    (void)len;
    uint32_t t = u24(d + 3);
    if (ok_u24(t)) set_temp(ctx, src, pgn, d[2], t * 0.001, now);
}

static const n2k_pgn_def pgn_table[] = {
    { 127233, 1, 30, n2k_mob_notification },
    { 127245, 0,  6, d127245 },
    { 127250, 0,  8, d127250 },
    { 127251, 0,  5, d127251 },
    { 127257, 0,  7, d127257 },
    { 127258, 0,  6, d127258 },
    { 128259, 0,  3, d128259 },
    { 128267, 0,  7, d128267 },
    { 128275, 1, 14, d128275 },
    { 129025, 0,  8, d129025 },
    { 129026, 0,  6, d129026 },
    { 129029, 1, 38, d129029 },
    { 129038, 1, 18, n2k_ais_position },
    { 129039, 1, 18, n2k_ais_position },
    { 129283, 0,  6, d129283 },
    { 129284, 1, 34, d129284 },
    { 129285, 1, 10, d129285 },
    { 129291, 0,  6, d129291 },
    { 129539, 0,  4, d129539 },
    { 129802, 1,  8, n2k_ais_safety },
    { 129794, 1, 36, n2k_ais_static_a },
    { 129809, 1, 25, n2k_ais_static_b },
    { 130306, 0,  6, d130306 },
    { 130311, 0,  8, d130311 },
    { 130312, 0,  5, d130312 },
    { 130314, 0,  7, d130314 },
    { 130316, 0,  6, d130316 },
};

const n2k_pgn_def *n2k_find_pgn(int pgn)
{
    for (size_t i = 0; i < sizeof pgn_table / sizeof pgn_table[0]; i++)
        if (pgn_table[i].pgn == pgn)
            return &pgn_table[i];
    return NULL;
}
