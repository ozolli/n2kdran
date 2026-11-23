/*
 * ais.c — Cibles AIS : table des cibles, cible la plus menaçante (CPA/TCPA),
 * balises de détresse (AIS-SART, MOB, EPIRB).
 *
 * Champs (canboat docs/canboat.json) :
 *   129038 Class A Position Report (fast, 28) et 129039 Class B Position Report
 *   (fast, 27) : messageId 6 bits @0, MMSI u32 @1, longitude s32 @5, latitude
 *   s32 @9 (1e-7 °), COG u16 @14 (1e-4 rad), SOG u16 @16 (0,01 m/s) ;
 *   129038 seulement : navStatus 4 bits @25.
 *   129794 Class A Static and Voyage (fast, 76) : MMSI @1, nom 20 car. @16.
 *   129809 Class B Static Data Part A (fast, 27) : MMSI @1, nom 20 car. @5.
 *   127233 Man Overboard Notification (fast, 35) : identifiant d'émetteur u32 @1,
 *   statut 3 bits @5 (0 émetteur activé, 1 bouton MOB du bord, 2 test, 3 inactif),
 *   latitude s32 @17, longitude s32 @21 (1e-7 °), COG u16 @26, SOG u16 @28.
 * Les noms AIS sont en ASCII 6 bits, complétés par '@' ou des espaces.
 */
#include "n2k_internal.h"

#include <math.h>
#include <string.h>

#define N2K_PI 3.14159265358979323846

static uint32_t u32le(const uint8_t *d)
{
    return (uint32_t)d[0] | (uint32_t)d[1] << 8 | (uint32_t)d[2] << 16 | (uint32_t)d[3] << 24;
}

static int distress_kind(const n2k_ais_slot *s, uint64_t now);

/* Homme à la mer (MOB AIS actif ou 127233) : mémorisé jusqu'à n2k_ais_reset,
 * ni périmé, ni désactivé par le bus, ni remplacé quand la table est pleine. */
static int latched(const n2k_ais_slot *s, uint64_t now)
{
    int k = s->mmsi && s->t ? distress_kind(s, now) : 0;     /* t = 0 : case qui vient d'être créée */
    return k == 972 || k == N2K_MOB_N2K;
}

/* Cible de ce MMSI (ou de cet émetteur MOB si mob), sinon une case libre, sinon la plus ancienne. */
static n2k_ais_slot *slot_for(n2k_ctx *ctx, uint32_t mmsi, int mob, uint64_t now)
{
    n2k_ais_slot *oldest = NULL;
    for (int i = 0; i < N2K_AIS_MAX; i++) {
        n2k_ais_slot *s = &ctx->ais[i];
        if (s->mmsi == mmsi && s->mob == mob)
            return s;
        if (!s->mmsi) {
            oldest = s;
            break;
        }
        if (latched(s, now))
            continue;
        uint64_t ts = s->t > s->name_t ? s->t : s->name_t;
        uint64_t to = oldest ? (oldest->t > oldest->name_t ? oldest->t : oldest->name_t) : UINT64_MAX;
        if (ts < to)
            oldest = s;
    }
    if (!oldest)
        oldest = &ctx->ais[N2K_AIS_MAX - 1];      /* table entière de MOB : improbable */
    memset(oldest, 0, sizeof *oldest);
    oldest->mmsi = mmsi;
    oldest->mob = mob;
    return oldest;
}

/* Balise de détresse d'après le MMSI : 970 AIS-SART, 972 MOB, 974 EPIRB-AIS. */
static int distress_mmsi(uint32_t mmsi)
{
    unsigned pfx = mmsi / 1000000u;
    return pfx == 970 || pfx == 972 || pfx == 974;
}

/* Homme à la mer sans position (équipier tombé avec sa balise MOB, avant son point
 * GNSS ; 127233 sans position) : placé, faute de mieux, à la position du bateau au
 * moment où l'alerte est reçue (immobile). Sans position du bateau, il reste inconnu
 * et l'on réessaie au message suivant. Pas pour SART et EPIRB, souvent reçues de
 * loin : l'estimation serait fausse de plusieurs milles. */
static void estimate(const n2k_ctx *ctx, n2k_ais_slot *s, uint64_t now)
{
    double lat, lon;
    if (!s->mob && s->mmsi / 1000000u != 972)
        return;
    if (s->est || !isnan(s->lat))
        return;
    if (n2k_get(ctx, N2K_LAT, now, &lat) != N2K_OK || n2k_get(ctx, N2K_LON, now, &lon) != N2K_OK)
        return;
    s->lat = lat;
    s->lon = lon;
    s->est = 1;
}

/* 129038 / 129039 : position, route et vitesse de la cible. Une balise de détresse
 * sans point GNSS (premiers messages après l'activation) est gardée sans position :
 * l'alerte part tout de suite, la dernière position connue est conservée. */
void n2k_ais_position(n2k_ctx *ctx, int src, int pgn, const uint8_t *d, int len, uint64_t now)
{
    (void)src;
    uint32_t mmsi = u32le(d + 1);
    if (!mmsi || mmsi > 999999999u)
        return;
    int32_t lon = (int32_t)u32le(d + 5), lat = (int32_t)u32le(d + 9);
    int pos_ok = lat >= -900000000 && lat <= 900000000 && lon >= -1800000000 && lon <= 1800000000;
    if (!pos_ok && !distress_mmsi(mmsi))
        return;                                    /* position indisponible */
    unsigned cog = (unsigned)d[14] | (unsigned)d[15] << 8;
    unsigned sog = (unsigned)d[16] | (unsigned)d[17] << 8;
    n2k_ais_slot *s = slot_for(ctx, mmsi, 0, now);
    if (pos_ok) {
        s->lat = lat * 1e-7;
        s->lon = lon * 1e-7;
        s->est = 0;
    } else if (!s->t) {
        s->lat = s->lon = NAN;                     /* jamais eu de position */
    }
    /* Route ou vitesse indisponible : cible traitée comme immobile. */
    s->sog = (sog <= 0xFFFCu && cog <= 0xFFFCu) ? sog * 0.01 : 0.0;
    s->cog = cog <= 0xFFFCu ? cog * 1e-4 : 0.0;
    if (!pos_ok) {
        estimate(ctx, s, now);
        if (s->est)
            s->sog = s->cog = 0;
    }
    if (s->test_until && now < s->test_until)
        s->nav_status = 15;                        /* annoncée en test (129802) */
    else if (!latched(s, now))                     /* MOB actif : plus de retour en test */
        s->nav_status = (pgn == 129038 && len >= 26) ? (d[25] & 0x0F) : -1;
    s->t = now;
}

/* 129802 AIS Safety Related Broadcast (message AIS 14) : MMSI @1, texte LAU @6
 * ([longueur][codage][octets]). Les balises émettent « SART TEST », « MOB TEST »,
 * « EPIRB TEST » en test, « … ACTIVE » en détresse. Un texte TEST d'une balise de
 * détresse la fait ignorer 10 min (alerte mémorisée levée) ; ACTIVE annule le test. */
void n2k_ais_safety(n2k_ctx *ctx, int src, int pgn, const uint8_t *d, int len, uint64_t now)
{
    (void)src; (void)pgn;
    uint32_t mmsi = u32le(d + 1);
    if (!distress_mmsi(mmsi) || d[6] < 2)
        return;
    int n = d[6] - 2;
    if (n > len - 8)
        n = len - 8;
    char txt[64];
    int k = 0;
    for (int i = 0; i < n && k < (int)sizeof txt - 1; i++) {
        uint8_t c = d[8 + i];
        txt[k++] = (char)(c >= 'a' && c <= 'z' ? c - 32 : c);
    }
    txt[k] = 0;
    int test = strstr(txt, "TEST") != NULL;
    if (!test && !strstr(txt, "ACTIVE"))
        return;
    n2k_ais_slot *s = slot_for(ctx, mmsi, 0, now);
    if (test) {
        s->test_until = now + N2K_AIS_TEST_MS;
        s->nav_status = 15;                        /* plus d'alerte mémorisée à la fin du délai */
    } else {
        s->test_until = 0;
    }
}

/* 127233 : homme à la mer signalé sur le bus (émetteur MOB ou bouton du bord).
 * Une fois actif, mémorisé jusqu'à n2k_ais_reset : les statuts « test » et
 * « inactif » qui suivent ne le retirent pas. Position absente : dernière connue,
 * sinon celle du bateau à la réception (estimate). */
void n2k_mob_notification(n2k_ctx *ctx, int src, int pgn, const uint8_t *d, int len, uint64_t now)
{
    (void)src; (void)pgn; (void)len;
    uint32_t id = u32le(d + 1);
    if (!id)
        id = 0xFFFFFFFFu;                          /* 0 marque une case libre */
    int status = d[5] & 0x07;
    if (status == 3)                               /* inactif : rien à signaler */
        return;
    n2k_ais_slot *s = slot_for(ctx, id, 1, now);
    if (status == 2 && latched(s, now))
        return;                                    /* MOB actif : on le garde */
    int32_t lat = (int32_t)u32le(d + 17), lon = (int32_t)u32le(d + 21);
    int pos_ok = !(lat < -900000000 || lat > 900000000 || lon < -1800000000 || lon > 1800000000);
    if (pos_ok) {
        s->lat = lat * 1e-7;
        s->lon = lon * 1e-7;
        s->est = 0;
    } else if (!s->t) {
        s->lat = s->lon = NAN;                     /* jamais eu de position */
    }
    unsigned cog = (unsigned)d[26] | (unsigned)d[27] << 8;
    unsigned sog = (unsigned)d[28] | (unsigned)d[29] << 8;
    s->sog = (sog <= 0xFFFCu && cog <= 0xFFFCu) ? sog * 0.01 : 0.0;
    s->cog = cog <= 0xFFFCu ? cog * 1e-4 : 0.0;
    if (!pos_ok) {
        estimate(ctx, s, now);
        if (s->est)
            s->sog = s->cog = 0;
    }
    s->nav_status = status == 2 ? 15 : 14;         /* test : ignoré comme une balise AIS en test */
    s->t = now;
}

/* Nom ASCII 6 bits : '@' et espaces de fin retirés, caractères non imprimables ignorés. */
static void set_name(n2k_ctx *ctx, const uint8_t *d, const uint8_t *name, uint64_t now)
{
    uint32_t mmsi = u32le(d + 1);
    if (!mmsi || mmsi > 999999999u)
        return;
    char buf[N2K_AIS_NAME];
    int n = 0;
    for (int i = 0; i < N2K_AIS_NAME - 1; i++) {
        uint8_t c = name[i];
        if (c == '@' || c == 0 || c == 0xFF)
            break;
        if (c >= 0x20 && c < 0x7F)
            buf[n++] = (char)c;
    }
    while (n > 0 && buf[n - 1] == ' ')
        n--;
    buf[n] = 0;
    if (!n)
        return;
    n2k_ais_slot *s = slot_for(ctx, mmsi, 0, now);
    memcpy(s->name, buf, sizeof buf);
    s->name_t = now;
}

void n2k_ais_static_a(n2k_ctx *ctx, int src, int pgn, const uint8_t *d, int len, uint64_t now)
{
    (void)src; (void)pgn; (void)len;
    set_name(ctx, d, d + 16, now);
}

void n2k_ais_static_b(n2k_ctx *ctx, int src, int pgn, const uint8_t *d, int len, uint64_t now)
{
    (void)src; (void)pgn; (void)len;
    set_name(ctx, d, d + 5, now);
}

/* Balise de détresse : MMSI 970xxxxxx (AIS-SART), 972 (MOB), 974 (EPIRB-AIS).
 * Statut de navigation 15 = mode test : ignorée. */
static int distress_kind(const n2k_ais_slot *s, uint64_t now)
{
    if (s->test_until && now < s->test_until)
        return 0;                                  /* annoncée en test (129802) */
    if (s->mob)
        return s->nav_status == 15 ? 0 : N2K_MOB_N2K;
    unsigned pfx = s->mmsi / 1000000u;
    if (pfx != 970 && pfx != 972 && pfx != 974)
        return 0;
    if (s->nav_status == 15)
        return 0;
    return (int)pfx;
}

/* Position propre fraîche ; vitesse fond (E, N) en m/s, nulle si absente. */
static int own_state(const n2k_ctx *ctx, uint64_t now, double *lat, double *lon,
                     double *ve, double *vn)
{
    if (n2k_get(ctx, N2K_LAT, now, lat) != N2K_OK || n2k_get(ctx, N2K_LON, now, lon) != N2K_OK)
        return 0;
    double cog, sog;
    *ve = *vn = 0;
    if (n2k_get(ctx, N2K_COG, now, &cog) == N2K_OK && n2k_get(ctx, N2K_SOG, now, &sog) == N2K_OK) {
        *ve = sog * sin(cog);
        *vn = sog * cos(cog);
    }
    return 1;
}

/* Remplit `out` pour la cible `s` : position relative (m, plan local), relèvement,
 * distance, CPA et TCPA. La cible est extrapolée à `now` avec sa route et sa vitesse. */
static void relative(const n2k_ais_slot *s, uint64_t now, double lat, double lon,
                     double ve, double vn, n2k_ais_target *out)
{
    double age = (double)(now - s->t) / 1000.0;
    double tve = s->sog * sin(s->cog), tvn = s->sog * cos(s->cog);
    double m_deg = 60.0 * 1852.0;
    double x = (s->lon - lon) * m_deg * cos(lat * N2K_PI / 180.0) + tve * age;
    double y = (s->lat - lat) * m_deg + tvn * age;
    double rx = tve - ve, ry = tvn - vn;
    double v2 = rx * rx + ry * ry;
    memset(out, 0, sizeof *out);
    out->mmsi = s->mmsi;
    memcpy(out->name, s->name, sizeof out->name);
    out->range = sqrt(x * x + y * y);
    out->bearing = atan2(x, y);
    if (out->bearing < 0)
        out->bearing += 2 * N2K_PI;
    out->rel_speed = sqrt(v2);
    if (v2 > 1e-6) {
        out->tcpa = -(x * rx + y * ry) / v2;
        double cx = x + rx * out->tcpa, cy = y + ry * out->tcpa;
        out->cpa = sqrt(cx * cx + cy * cy);
    } else {
        out->tcpa = 0;
        out->cpa = out->range;
    }
    out->age_ms = now - s->t;
    out->est = s->est;
}

static int skipped(uint32_t mmsi, const uint32_t *skip, int nskip)
{
    for (int i = 0; i < nskip; i++)
        if (skip[i] == mmsi)
            return 1;
    return 0;
}

static int fresh(const n2k_ais_slot *s, uint64_t now)
{
    return s->mmsi && s->t && now >= s->t && now - s->t <= N2K_AIS_TTL_MS;
}

int n2k_ais_threat(const n2k_ctx *ctx, uint64_t now, double cpa_max, double tcpa_max,
                   double range_max, const uint32_t *skip, int nskip, n2k_ais_target *out)
{
    double lat, lon, ve, vn;
    if (!own_state(ctx, now, &lat, &lon, &ve, &vn))
        return 0;
    int best = 0;                                   /* 0 aucune, 1 suivie, 2 dangereuse */
    n2k_ais_target t, b;
    for (int i = 0; i < N2K_AIS_MAX; i++) {
        const n2k_ais_slot *s = &ctx->ais[i];
        if (!fresh(s, now) || s->mob || distress_mmsi(s->mmsi) || skipped(s->mmsi, skip, nskip))
            continue;
        relative(s, now, lat, lon, ve, vn, &t);
        if (range_max > 0 && t.range > range_max)
            continue;                               /* trop loin pour être surveillée */
        /* Mouvement relatif trop lent (bateaux au mouillage, à quai…), cible qui
         * s'éloigne ou rapprochement trop lointain : pas de menace. */
        if (t.rel_speed < N2K_AIS_MIN_REL || t.tcpa < 0 || t.tcpa > tcpa_max)
            continue;
        int lvl = t.cpa < cpa_max ? 2 : 1;
        if (ctx->ais_ignore_still && s->sog < N2K_AIS_MIN_REL)
            lvl = 1;                                /* immobile : suivie, sans alarme */
        if (lvl > best || (lvl == best && (lvl == 2 ? t.tcpa < b.tcpa : t.cpa < b.cpa))) {
            best = lvl;
            b = t;
        }
    }
    if (best && out)
        *out = b;
    return best;
}

int n2k_ais_distress(const n2k_ctx *ctx, uint64_t now, const uint32_t *skip, int nskip,
                     n2k_ais_target *out)
{
    double lat, lon, ve, vn;
    int own = own_state(ctx, now, &lat, &lon, &ve, &vn);
    int best = 0, best_mob = 0;
    n2k_ais_target t, b;
    for (int i = 0; i < N2K_AIS_MAX; i++) {
        const n2k_ais_slot *s = &ctx->ais[i];
        int kind = (fresh(s, now) || latched(s, now)) && !skipped(s->mmsi, skip, nskip) ? distress_kind(s, now) : 0;
        if (!kind)
            continue;
        if (own) {
            relative(s, now, lat, lon, ve, vn, &t);
        } else {                                    /* sans position : distance inconnue */
            memset(&t, 0, sizeof t);
            t.mmsi = s->mmsi;
            memcpy(t.name, s->name, sizeof t.name);
            t.range = t.bearing = t.cpa = t.tcpa = NAN;
            t.age_ms = now - s->t;
        }
        /* Homme à la mer (MOB AIS ou 127233) d'abord, puis la plus proche. */
        int mob = kind == 972 || kind == N2K_MOB_N2K;
        if (!best || mob > best_mob || (mob == best_mob && own && t.range < b.range)) {
            best = kind;
            best_mob = mob;
            b = t;
        }
    }
    if (best && out)
        *out = b;
    return best;
}

void n2k_ais_reset(n2k_ctx *ctx)
{
    memset(ctx->ais, 0, sizeof ctx->ais);
}

int n2k_ais_count(const n2k_ctx *ctx, uint64_t now)
{
    int n = 0;
    for (int i = 0; i < N2K_AIS_MAX; i++)
        n += fresh(&ctx->ais[i], now) && !ctx->ais[i].mob;
    return n;
}
