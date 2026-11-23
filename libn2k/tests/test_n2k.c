/*
 * test_n2k.c — tests unitaires de libn2k.
 * Usage : test_n2k [répertoire tests/data]
 */
#include "n2k.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0, checks = 0;

#define CHECK(cond) do { checks++; if (!(cond)) { fails++; \
    fprintf(stderr, "%s:%d: ÉCHEC : %s\n", __FILE__, __LINE__, #cond); } } while (0)

#define CHECK_NEAR(a, b, eps) do { double _a = (a), _b = (b); checks++; \
    if (!(fabs(_a - _b) <= (eps))) { fails++; \
    fprintf(stderr, "%s:%d: ÉCHEC : %s = %.9g, attendu %.9g\n", __FILE__, __LINE__, #a, _a, _b); } } while (0)

static const double TWO_PI = 6.283185307179586;

static double get(n2k_ctx *c, n2k_item it, uint64_t now)
{
    double v = NAN;
    if (n2k_get(c, it, now, &v) != N2K_OK)
        return NAN;
    return v;
}

static void test_parse(void)
{
    uint32_t tod, id;
    uint8_t d[8];
    int len;

    CHECK(n2k_parse_ydraw("12:34:56.789 R 09F80103 00 FF FF 7F FF 7F FF FF\r\n", &tod, &id, d, &len));
    CHECK(tod == ((12u * 60 + 34) * 60 + 56) * 1000 + 789);
    CHECK(id == 0x09F80103u && len == 8 && d[0] == 0x00 && d[3] == 0x7F && d[7] == 0xFF);

    CHECK(n2k_parse_ydraw("00:00:00.000 R 1 ab", &tod, &id, d, &len) && id == 1 && len == 1 && d[0] == 0xAB);
    CHECK(n2k_parse_ydraw("23:59:59.999 R 0DF01002\n", &tod, &id, d, &len) && len == 0);
    CHECK(n2k_parse_ydraw("23:59:59.999 T 0DF01002 01  \r\n", &tod, &id, d, &len) && len == 1);

    const char *bad[] = {
        "", "\r\n", "garbage", "12:34:56.789", "12:34:56.789 R",
        "12:34:56.789 X 09F80103 00", "12:34:56.789 R 09F8010G 00",
        "12:34:56.789 R 20000000 00",                       /* > 29 bits */
        "12:34:56.789 R 109F80103 00",                      /* 9 chiffres */
        "12:34:56.789 R 09F80103 00 11 22 33 44 55 66 77 88", /* 9 octets */
        "12:34:56.789 R 09F80103 0",                        /* octet incomplet */
        "12:34:56.789 R 09F80103 000",
        "12:34:56.789 R 09F80103 00 ZZ",
        "12:34:56.789 R 09F80103 00 *",
        "24:00:00.000 R 09F80103 00",
        "12:34:56.78 R 09F80103 00",
        "12:34:56.789R 09F80103 00",
        "12:34:56.789 R 09F80103 00\r\nX",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        int r = n2k_parse_ydraw(bad[i], &tod, &id, d, &len);
        if (r)
            fprintf(stderr, "ligne invalide acceptée : \"%s\"\n", bad[i]);
        CHECK(!r);
    }
    CHECK(!n2k_parse_ydraw(NULL, &tod, &id, d, &len));
}

static void test_id(void)
{
    int prio, pgn, src, dst;
    n2k_decode_id(0x09F80202u, &prio, &pgn, &src, &dst);
    CHECK(prio == 2 && pgn == 129026 && src == 2 && dst == 255);
    n2k_decode_id(0x11F811B2u, &prio, &pgn, &src, &dst);
    CHECK(prio == 4 && pgn == 129041 && src == 178 && dst == 255);
    n2k_decode_id(0x18EA0001u, &prio, &pgn, &src, &dst);     /* PDU1 : ISO Request */
    CHECK(prio == 6 && pgn == 59904 && src == 1 && dst == 0);
}

/* Trames réelles relevées dans tests/data/sim-30s.ydraw. */
static void test_decoders(void)
{
    n2k_ctx *c = n2k_create();
    uint64_t t = 1000;

    CHECK(n2k_feed_line(c, "12:28:51.037 R 09FD0205 FF 42 05 3A DB FA FF FF", t));
    CHECK(n2k_feed_line(c, "12:28:51.037 R 09FD0205 FF 9F 03 7C C9 FC FF FF", t));
    CHECK_NEAR(get(c, N2K_AWS, t), 13.46, 1e-9);
    CHECK_NEAR(get(c, N2K_AWA, t), 5.6122 - TWO_PI, 1e-9);      /* -38,4° bâbord */
    CHECK_NEAR(get(c, N2K_TWS, t), 9.27, 1e-9);
    CHECK_NEAR(get(c, N2K_TWA, t), 5.1580 - TWO_PI, 1e-9);      /* -64,5° bâbord */

    CHECK(n2k_feed_line(c, "12:28:50.937 R 09F11204 FF 38 64 00 00 A3 FE FC", t));
    CHECK_NEAR(get(c, N2K_HDG_TRUE, t), 2.5656, 1e-9);          /* 147,0° */
    CHECK_NEAR(get(c, N2K_VARIATION, t), -0.0349, 1e-9);        /* -2,0° */
    CHECK(n2k_get(c, N2K_HDG_MAG, t, NULL) == N2K_NEVER);

    CHECK(n2k_feed_line(c, "12:28:51.537 R 15FD0A04 FF 00 00 76 76 0F 00 FF", t));
    CHECK_NEAR(get(c, N2K_PRESSURE, t), 101336.6, 1e-6);
    CHECK(n2k_feed_line(c, "12:28:51.537 R 15FD0C14 FF 00 00 11 72 04 FF FF", t));
    CHECK_NEAR(get(c, N2K_SEA_TEMP, t), 291.345, 1e-9);
    CHECK(n2k_feed_line(c, "12:28:51.537 R 15FD0704 FF 01 6C 73 98 3A F5 03", t));
    CHECK_NEAR(get(c, N2K_AIR_TEMP, t), 295.48, 1e-9);
    CHECK_NEAR(get(c, N2K_HUMIDITY, t), 60.0, 1e-9);
    /* La pression de 130311 (même source, autre PGN) ne remplace pas 130314 frais. */
    CHECK_NEAR(get(c, N2K_PRESSURE, t), 101336.6, 1e-6);

    /* COG/SOG réel : ref vraie, COG 0,5728 rad, SOG 0,13 m/s. */
    CHECK(n2k_feed_line(c, "12:17:22.267 R 09F80202 6E FC 60 16 0D 00 FF FF", t));
    CHECK_NEAR(get(c, N2K_COG, t), 0.5728, 1e-9);
    CHECK_NEAR(get(c, N2K_SOG, t), 0.13, 1e-9);

    /* Position réelle : 0x1B9A7752, 0xFF68682C. */
    CHECK(n2k_feed_line(c, "12:17:22.268 R 09F80102 52 77 9A 1B 2C 68 68 FF", t));
    CHECK_NEAR(get(c, N2K_LAT, t), 463107922 * 1e-7, 1e-12);
    CHECK_NEAR(get(c, N2K_LON, t), (double)(int32_t)0xFF68682Cu * 1e-7, 1e-12);

    n2k_destroy(c);
}

static void test_not_available(void)
{
    n2k_ctx *c = n2k_create();
    /* Vent apparent : vitesse et angle « indisponibles » (FFFF, FFFD). */
    n2k_feed_line(c, "00:00:00.000 R 09FD0205 FF FF FF FD FF FA FF FF", 0);
    CHECK(n2k_get(c, N2K_AWS, 0, NULL) == N2K_NEVER);
    CHECK(n2k_get(c, N2K_AWA, 0, NULL) == N2K_NEVER);
    /* 0xFFFC est encore une valeur valide (655,32 m/s) — limite exacte. */
    n2k_feed_line(c, "00:00:00.000 R 09FD0205 FF FC FF 00 00 FA FF FF", 0);
    CHECK_NEAR(get(c, N2K_AWS, 0), 655.32, 1e-9);
    /* Position indisponible (7FFFFFFF). */
    n2k_feed_line(c, "00:00:00.000 R 09F80102 FF FF FF 7F FF FF FF 7F", 0);
    CHECK(n2k_get(c, N2K_LAT, 0, NULL) == N2K_NEVER);
    /* Trame trop courte pour son PGN : comptée, sans effet. */
    n2k_feed_line(c, "00:00:00.000 R 09F80102 52 77 9A", 0);
    CHECK(n2k_get(c, N2K_LAT, 0, NULL) == N2K_NEVER);
    n2k_stats s;
    n2k_get_stats(c, &s);
    CHECK(s.decode_errors == 1);
    n2k_destroy(c);
}

/* 129029 réel (src 2, séquence 2, 43 octets, 7 trames). */
static const char *const gnss[] = {
    "12:17:22.787 R 0DF80502 40 2B 73 20 51 20 7A 39",
    "12:17:22.801 R 0DF80502 41 18 00 00 00 00 28 4A",
    "12:17:22.807 R 0DF80502 42 6D 06 00 00 00 00 55",
    "12:17:22.817 R 0DF80502 43 B4 DC FF A0 E8 32 03",
    "12:17:22.827 R 0DF80502 44 00 00 00 00 14 FC 18",
    "12:17:22.837 R 0DF80502 45 6C 00 AF 00 45 ED FF",
    "12:17:22.847 R 0DF80502 46 FF FF FF FF FF FF FF",
};

static void test_fastpacket(void)
{
    n2k_ctx *c = n2k_create();
    n2k_stats s;

    for (int i = 0; i < 7; i++)
        n2k_feed_line(c, gnss[i], 100 + (uint64_t)i * 10);
    CHECK_NEAR(get(c, N2K_GNSS_METHOD, 200), 1, 0);   /* GNSS fix */
    CHECK_NEAR(get(c, N2K_GNSS_SATS, 200), 24, 0);
    CHECK_NEAR(get(c, N2K_HDOP, 200), 1.08, 1e-9);
    CHECK_NEAR(get(c, N2K_PDOP, 200), 1.75, 1e-9);
    n2k_get_stats(c, &s);
    CHECK(s.fp_complete == 1 && s.fp_dropped == 0);
    n2k_destroy(c);

    /* Trame 3 perdue : message abandonné, rien de décodé. */
    c = n2k_create();
    for (int i = 0; i < 7; i++)
        if (i != 3)
            n2k_feed_line(c, gnss[i], 100);
    CHECK(n2k_get(c, N2K_GNSS_SATS, 100, NULL) == N2K_NEVER);
    n2k_get_stats(c, &s);
    CHECK(s.fp_complete == 0 && s.fp_dropped == 1);
    n2k_destroy(c);

    /* Compteur de séquence qui change en cours de message. */
    c = n2k_create();
    n2k_feed_line(c, gnss[0], 100);
    n2k_feed_line(c, gnss[1], 100);
    n2k_feed_line(c, "12:17:22.807 R 0DF80502 62 6D 06 00 00 00 00 55", 100);
    for (int i = 3; i < 7; i++)
        n2k_feed_line(c, gnss[i], 100);
    CHECK(n2k_get(c, N2K_GNSS_SATS, 100, NULL) == N2K_NEVER);
    n2k_destroy(c);

    /* Message trop lent (> 750 ms entre trame 0 et la dernière). */
    c = n2k_create();
    for (int i = 0; i < 7; i++)
        n2k_feed_line(c, gnss[i], 100 + (uint64_t)i * 200);
    CHECK(n2k_get(c, N2K_GNSS_SATS, 2000, NULL) == N2K_NEVER);
    n2k_destroy(c);

    /* Deux sources entrelacées (src 2 et src 7) : les deux messages aboutissent. */
    c = n2k_create();
    char alt[64];
    for (int i = 0; i < 7; i++) {
        n2k_feed_line(c, gnss[i], 100);
        strcpy(alt, gnss[i]);
        memcpy(alt + 21, "07", 2);           /* 0DF80502 → 0DF80507 */
        n2k_feed_line(c, alt, 100);
    }
    n2k_get_stats(c, &s);
    CHECK(s.fp_complete == 2 && s.fp_dropped == 0);
    n2k_destroy(c);

    /* Reprise propre après une trame 0 sans suite. */
    c = n2k_create();
    n2k_feed_line(c, gnss[0], 100);
    n2k_feed_line(c, gnss[1], 100);
    for (int i = 0; i < 7; i++)
        n2k_feed_line(c, gnss[i], 100);
    CHECK_NEAR(get(c, N2K_GNSS_SATS, 100), 24, 0);
    n2k_destroy(c);
}

static void test_sources(void)
{
    n2k_ctx *c = n2k_create();
    n2k_set_stale_ms(c, 3000);
    int src;

    /* 128259 BSP : src 5 = 5,00 m/s, src 9 = 2,00 m/s. */
    n2k_feed_line(c, "00:00:00.000 R 09F50305 FF F4 01 FF FF 00 FF FF", 0);
    n2k_feed_line(c, "00:00:00.000 R 09F50309 FF C8 00 FF FF 00 FF FF", 1000);
    CHECK_NEAR(get(c, N2K_BSP, 1000), 5.00, 1e-9);           /* src 5 garde la main */
    CHECK(n2k_get(c, N2K_BSP, 3000, NULL) == N2K_OK);
    CHECK(n2k_get(c, N2K_BSP, 3001, NULL) == N2K_STALE);     /* jamais figée */
    n2k_feed_line(c, "00:00:00.000 R 09F50309 FF C8 00 FF FF 00 FF FF", 3001);
    CHECK_NEAR(get(c, N2K_BSP, 3001), 2.00, 1e-9);           /* bascule sur src 9 */
    n2k_get_info(c, N2K_BSP, 3001, &src, NULL, NULL);
    CHECK(src == 9);
    n2k_destroy(c);
}

static void test_garbage(void)
{
    n2k_ctx *c = n2k_create();
    char line[80];
    uint32_t x = 12345;
    for (int n = 0; n < 200000; n++) {
        int l = (int)(x % 79);
        for (int i = 0; i < l; i++) {
            x = x * 1103515245u + 12345u;
            line[i] = (char)(x >> 16);
            if (line[i] == 0) line[i] = ' ';
        }
        line[l] = 0;
        n2k_feed_line(c, line, (uint64_t)n);
        /* Trames syntaxiquement valides au contenu aléatoire, PGN décodés. */
        static const char *ids[] = { "0DF80502", "09FD0205", "09F11204", "19F51314", "09F80102" };
        x = x * 1103515245u + 12345u;
        int k = snprintf(line, sizeof line, "00:00:00.000 R %s", ids[(x >> 16) % 5]);
        int nb = (int)((x >> 8) % 9);
        for (int i = 0; i < nb; i++) {
            x = x * 1103515245u + 12345u;
            k += snprintf(line + k, sizeof line - (size_t)k, " %02X", (x >> 16) & 0xFF);
        }
        n2k_feed_line(c, line, (uint64_t)n);
    }
    double v[N2K_ITEM_COUNT];
    uint8_t st[N2K_ITEM_COUNT];
    n2k_snapshot(c, 200000, v, st);
    CHECK(1);   /* survivre suffit ; ASan/UBSan font le reste */
    n2k_destroy(c);
}

/* Rejeu d'une capture avec le temps des horodatages. */
static n2k_ctx *replay(const char *path, uint64_t *end)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "impossible d'ouvrir %s\n", path);
        fails++;
        return NULL;
    }
    n2k_ctx *c = n2k_create();
    char line[256];
    uint64_t now = 0;
    while (fgets(line, sizeof line, f)) {
        uint32_t tod, id;
        uint8_t d[8];
        int len;
        if (n2k_parse_ydraw(line, &tod, &id, d, &len))
            now = tod;
        n2k_feed_line(c, line, now);
    }
    fclose(f);
    *end = now;
    return c;
}

static void test_captures(const char *sim, const char *reel)
{
    uint64_t end;
    n2k_stats s;

    n2k_ctx *c = replay(sim, &end);
    if (c) {
        n2k_get_stats(c, &s);
        CHECK(s.bad_lines == 0 && s.decode_errors == 0);
        CHECK(s.fp_complete > 0);
        static const n2k_item expect[] = {
            N2K_LAT, N2K_LON, N2K_COG, N2K_SOG, N2K_HDG_TRUE, N2K_VARIATION,
            N2K_ROT, N2K_PITCH, N2K_ROLL, N2K_RUDDER, N2K_BSP, N2K_DEPTH,
            N2K_LOG, N2K_TRIP, N2K_AWA, N2K_AWS, N2K_TWA, N2K_TWS, N2K_SET,
            N2K_DRIFT, N2K_SEA_TEMP, N2K_AIR_TEMP, N2K_PRESSURE, N2K_HUMIDITY,
            N2K_GNSS_METHOD, N2K_GNSS_SATS, N2K_HDOP, N2K_PDOP,
        };
        for (size_t i = 0; i < sizeof expect / sizeof expect[0]; i++) {
            int ok = n2k_get(c, expect[i], end, NULL) == N2K_OK;
            if (!ok)
                fprintf(stderr, "sim : %s absent ou périmé\n", n2k_item_name(expect[i]));
            CHECK(ok);
        }
        n2k_destroy(c);
    }

    c = replay(reel, &end);
    if (c) {
        n2k_get_stats(c, &s);
        CHECK(s.bad_lines == 0 && s.decode_errors == 0);
        CHECK(s.frames_ignored > s.frames_used);   /* l'AIS domine et est filtré */
        CHECK(n2k_get(c, N2K_LAT, end, NULL) == N2K_OK);
        CHECK(n2k_get(c, N2K_GNSS_SATS, end, NULL) == N2K_OK);
        CHECK(n2k_get(c, N2K_AWA, end, NULL) == N2K_NEVER);
        fprintf(stderr, "réel : fp_ok=%llu fp_abandonnés=%llu\n",
                (unsigned long long)s.fp_complete, (unsigned long long)s.fp_dropped);
        n2k_destroy(c);
    }
}


static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "impossible d'ouvrir %s\n", path); fails++; return NULL; }
    static char buf[1 << 16];
    *len = fread(buf, 1, sizeof buf, f);
    fclose(f);
    return buf;
}

static const double KN = 1852.0 / 3600.0, DEG = 3.14159265358979323846 / 180.0;

static void test_polar(const char *dir)
{
    char path[512], err[128];
    size_t len;
    char *txt;

    snprintf(path, sizeof path, "%s/CM50.pol", dir);
    if (!(txt = slurp(path, &len)))
        return;
    n2k_polar *p = n2k_polar_parse(txt, len, err, sizeof err);
    CHECK(p != NULL);
    if (!p) { fprintf(stderr, "CM50 : %s\n", err); return; }

    /* Points de grille et interpolation bilinéaire. */
    CHECK_NEAR(n2k_polar_speed(p, 90 * DEG, 10 * KN) / KN, 9.80, 1e-9);
    CHECK_NEAR(n2k_polar_speed(p, -90 * DEG, 10 * KN) / KN, 9.80, 1e-9);   /* symétrie */
    CHECK_NEAR(n2k_polar_speed(p, 270 * DEG, 10 * KN) / KN, 9.80, 1e-9);
    CHECK_NEAR(n2k_polar_speed(p, 95 * DEG, 9 * KN) / KN, (8.2 + 9.8 + 7.9 + 9.4) / 4, 1e-9);
    CHECK_NEAR(n2k_polar_speed(p, 90 * DEG, 2 * KN) / KN, 3.9 / 2, 1e-9);  /* entre TWS 0 et 4 */
    /* Colonne TWS 60 nulle : bornage à 40 kn, pas d'interpolation vers 0. */
    CHECK_NEAR(n2k_polar_speed(p, 90 * DEG, 50 * KN) / KN, 23.4, 1e-9);
    CHECK_NEAR(n2k_polar_speed(p, 90 * DEG, 80 * KN) / KN, 23.4, 1e-9);
    CHECK(isnan(n2k_polar_speed(p, NAN, 10 * KN)));
    CHECK(isnan(n2k_polar_speed(NULL, 1, 10 * KN)));

    /* Cibles TWS 10 kn : au près ~38,5° (VMG ≈ 5,596 kn), au portant ~160°. */
    double a, v;
    CHECK(n2k_polar_target(p, 10 * KN, 1, &a, &v));
    CHECK(a / DEG >= 37 && a / DEG <= 40);
    CHECK(v * cos(a) / KN >= 5.592);                  /* ≥ meilleur point de grille (40°) */
    CHECK_NEAR(v, n2k_polar_speed(p, a, 10 * KN), 1e-12);
    CHECK(n2k_polar_target(p, 10 * KN, 0, &a, &v));
    CHECK(a / DEG >= 150 && a / DEG <= 170);
    CHECK(-v * cos(a) / KN >= 6.48);                  /* ≥ meilleur point de grille (160°) */
    CHECK(!n2k_polar_target(p, 0, 1, &a, &v));       /* pas de vent : pas de cible */

    /* Plage 99 % : à 16 kn la courbe de VMG au près a deux pics (40° et 50°). */
    double lo, hi;
    CHECK(n2k_polar_vmg_range(p, 16 * KN, 1, 0.99, &lo, &hi));
    CHECK(lo / DEG < 40 && hi / DEG > 50);
    CHECK(n2k_polar_vmg_range(p, 10 * KN, 1, 0.99, &lo, &hi));
    CHECK(lo / DEG < 38.6 && hi / DEG > 38.6 && hi / DEG < 45);
    CHECK(!n2k_polar_vmg_range(p, 10 * KN, 1, 1.5, &lo, &hi));

    /* Angle stabilisé : TWS oscillant entre 15,2 et 16,2 kn (zone bimodale). */
    for (int up = 1; up >= 0; up--) {
        n2k_target_state st = {0};
        double prev_raw = -1, prev_st = -1;
        int sw_raw = 0, sw_st = 0;
        for (int i = 0; i < 40; i++) {
            double w = (i % 2 ? 16.2 : 15.2) * KN, ar, as, vr, vs;
            n2k_polar_target(p, w, up, &ar, &vr);
            CHECK(n2k_polar_target_stable(p, w, up, 0.01, &st, &as, &vs));
            if (prev_raw >= 0 && fabs(ar - prev_raw) > 3 * DEG) sw_raw++;
            if (prev_st >= 0 && fabs(as - prev_st) > 3 * DEG) sw_st++;
            prev_raw = ar; prev_st = as;
            /* Jamais plus de 1 % de VMG perdu par rapport à l'optimum. */
            double vmg_opt = vr * cos(ar) * (up ? 1 : -1);
            double vmg_st = vs * cos(as) * (up ? 1 : -1);
            CHECK(vmg_st >= vmg_opt / 1.01 - 1e-9);
        }
        if (up) CHECK(sw_raw >= 30);          /* l'optimum brut bascule sans cesse */
        CHECK(sw_st == 0);                    /* l'angle stabilisé ne bascule pas */
    }

    /* Montée franche de la TWS : l'angle stabilisé finit par basculer (près). */
    {
        n2k_target_state st = {0};
        double as, vs;
        n2k_polar_target_stable(p, 12 * KN, 1, 0.01, &st, &as, &vs);
        CHECK(as / DEG < 40);
        for (double w = 12; w <= 20; w += 0.1)
            n2k_polar_target_stable(p, w * KN, 1, 0.01, &st, &as, &vs);
        CHECK(as / DEG > 48);
        for (double w = 20; w >= 12; w -= 0.1)
            n2k_polar_target_stable(p, w * KN, 1, 0.01, &st, &as, &vs);
        CHECK(as / DEG < 40);
    }
    n2k_polar_free(p);

    /* Variante tabulation + CRLF. */
    snprintf(path, sizeof path, "%s/figaro3-tab-crlf.pol", dir);
    if ((txt = slurp(path, &len))) {
        p = n2k_polar_parse(txt, len, err, sizeof err);
        CHECK(p != NULL);
        if (p) {
            CHECK_NEAR(n2k_polar_speed(p, 90 * DEG, 10 * KN) / KN, 7.1, 1e-9);
            CHECK_NEAR(n2k_polar_speed(p, 90 * DEG, 60 * KN) / KN, 0.6, 1e-9);
            n2k_polar_free(p);
        } else {
            fprintf(stderr, "figaro3 : %s\n", err);
        }
    }

    /* Sans ligne TWA 0 ni colonne TWS 0 : points implicites à 0. */
    const char *mini = "! commentaire\nTWA/TWS;10;20\n! autre\n60;6;8\n120;7;9\n";
    p = n2k_polar_parse(mini, strlen(mini), err, sizeof err);
    CHECK(p != NULL);
    if (p) {
        CHECK_NEAR(n2k_polar_speed(p, 30 * DEG, 10 * KN) / KN, 3.0, 1e-9);
        CHECK_NEAR(n2k_polar_speed(p, 60 * DEG, 5 * KN) / KN, 3.0, 1e-9);
        CHECK_NEAR(n2k_polar_speed(p, 170 * DEG, 20 * KN) / KN, 9.0, 1e-9); /* borne TWA */
        n2k_polar_free(p);
    }

    /* Fichiers invalides : refusés avec un message, jamais de plantage. */
    const char *bad[] = {
        "", "\n\n", "TWA\\TWS\n", "TWA\\TWS;4;6\n",
        "TWA\\TWS;6;4\n0;0;0\n",                 /* TWS décroissantes */
        "TWA\\TWS;4;6\n30;1;2;3\n",              /* colonnes en trop */
        "TWA\\TWS;4;6\n30;1\n",                  /* colonnes manquantes */
        "TWA\\TWS;4;6\n30;1;x\n",
        "TWA\\TWS;4;6\n30;1;2\n20;1;2\n",        /* TWA décroissant */
        "TWA\\TWS;4;6\n190;1;2\n",
        "TWA\\TWS;4;6\n30;-1;2\n",
        "TWA\\TWS;4;6\n30;0;0\n",                /* entièrement nulle */
        "TWA\\TWS;4,5;6\n30;1;2\n",              /* virgule décimale */
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        err[0] = 0;
        p = n2k_polar_parse(bad[i], strlen(bad[i]), err, sizeof err);
        if (p)
            fprintf(stderr, "polaire invalide acceptée : \"%s\"\n", bad[i]);
        CHECK(p == NULL && err[0] != 0);
        n2k_polar_free(p);
    }
}

/* --- Trames synthétiques pour les données calculées --- */
static void put16(uint8_t *d, unsigned v) { d[0] = (uint8_t)v; d[1] = (uint8_t)(v >> 8); }

static void wind(n2k_ctx *c, double ang, double spd, int ref, uint64_t t)
{
    uint8_t d[8] = { 0xFF, 0, 0, 0, 0, 0xF8, 0xFF, 0xFF };
    put16(d + 1, (unsigned)lround(spd / 0.01));
    put16(d + 3, (unsigned)lround(fmod(ang + 2 * 3.14159265358979323846, 2 * 3.14159265358979323846) / 1e-4));
    d[5] = (uint8_t)(0xF8 | ref);
    n2k_feed_frame(c, 0x09FD0205u, d, 8, t);
}

static void heading(n2k_ctx *c, double hdg, uint64_t t)
{
    uint8_t d[8] = { 0xFF, 0, 0, 0xFF, 0x7F, 0xFF, 0x7F, 0xFC };
    put16(d + 1, (unsigned)lround(hdg / 1e-4));
    n2k_feed_frame(c, 0x09F11204u, d, 8, t);
}

static void speed(n2k_ctx *c, double bsp, uint64_t t)
{
    uint8_t d[8] = { 0xFF, 0, 0, 0xFF, 0xFF, 0x00, 0xFF, 0xFF };
    put16(d + 1, (unsigned)lround(bsp / 0.01));
    n2k_feed_frame(c, 0x09F50305u, d, 8, t);
}

static void cogsog(n2k_ctx *c, double cog, double sog, uint64_t t)
{
    uint8_t d[8] = { 0xFF, 0xFC, 0, 0, 0, 0, 0xFF, 0xFF };
    put16(d + 2, (unsigned)lround(cog / 1e-4));
    put16(d + 4, (unsigned)lround(sog / 0.01));
    n2k_feed_frame(c, 0x09F80204u, d, 8, t);
}

static void setdrift(n2k_ctx *c, double set, double drift, uint64_t t)
{
    uint8_t d[8] = { 0xFF, 0xFC, 0, 0, 0, 0, 0xFF, 0xFF };
    put16(d + 2, (unsigned)lround(set / 1e-4));
    put16(d + 4, (unsigned)lround(drift / 0.01));
    n2k_feed_frame(c, 0x09F90B05u, d, 8, t);
}

static double wrapd(double a) { a = fmod(a, 2 * 3.14159265358979323846); return a < 0 ? a + 2 * 3.14159265358979323846 : a; }

static void test_derived(const char *dir)
{
    const double D = 3.14159265358979323846 / 180.0;
    n2k_ctx *c = n2k_create();
    double v;
    int src;

    /* TWD = cap + TWA ; VMG = BSP·cos(TWA). */
    heading(c, 100 * D, 0);
    speed(c, 6.0, 0);
    wind(c, -40 * D, 8.0, 4, 0);
    CHECK_NEAR(get(c, N2K_TWD, 0), 60 * D, 2e-4);
    CHECK_NEAR(get(c, N2K_VMG, 0), 6.0 * cos(40 * D), 1e-3);
    n2k_get_info(c, N2K_TWD, 0, &src, NULL, NULL);
    CHECK(src == N2K_SRC_CALC);
    CHECK(n2k_get(c, N2K_POLAR_BSP, 0, NULL) == N2K_NEVER);   /* pas de polaire */

    /* Le cap s'arrête : TWD périmé bien que le vent continue d'arriver. */
    for (uint64_t t = 500; t <= 7000; t += 500) {
        speed(c, 6.0, t);
        wind(c, -40 * D, 8.0, 4, t);
    }
    /* Cap frais jusqu'à 3000 ms → TWD recalculé jusque-là, périmé à 7000. */
    CHECK(n2k_get(c, N2K_TWD, 7000, NULL) == N2K_STALE);
    CHECK(n2k_get(c, N2K_VMG, 7000, NULL) == N2K_OK);
    n2k_destroy(c);

    /* Moyennes : TWS 5 m/s pendant 300 s puis 10 m/s pendant 300 s → 7,5 ;
     * TWD alternant 350° / 10° → moyenne vectorielle 0° (pas 180°). */
    c = n2k_create();
    for (uint64_t t = 0; t < 600000; t += 250) {
        heading(c, 0, t);
        double twd = ((t / 250) % 2) ? 10 * D : -10 * D;
        wind(c, twd, t < 300000 ? 5.0 : 10.0, 4, t);
    }
    CHECK_NEAR(get(c, N2K_TWS_AVG, 599750), 7.5, 0.02);
    v = get(c, N2K_TWD_AVG, 599750);
    CHECK(v < 0.5 * D || v > 359.5 * D);
    /* Fenêtre de 60 s : seule la seconde moitié compte. */
    n2k_set_windows(c, 60, 0, 0);
    wind(c, 0, 10.0, 4, 600000);
    CHECK_NEAR(get(c, N2K_TWS_AVG, 600000), 10.0, 0.02);
    n2k_destroy(c);

    /* Courant calculé, puis priorité à l'instrument 129291. */
    c = n2k_create();
    for (uint64_t t = 0; t <= 60000; t += 250) {
        heading(c, 90 * D, t);
        speed(c, 5.0, t);
        cogsog(c, 90 * D, 6.0, t);          /* 1 m/s de courant portant à l'Est */
    }
    CHECK_NEAR(get(c, N2K_DRIFT, 60000), 1.0, 0.01);
    CHECK_NEAR(get(c, N2K_SET, 60000), 90 * D, 0.5 * D);
    n2k_get_info(c, N2K_SET, 60000, &src, NULL, NULL);
    CHECK(src == N2K_SRC_CALC);
    setdrift(c, 200 * D, 0.30, 60100);       /* l'instrument prend la main tout de suite */
    cogsog(c, 90 * D, 6.0, 60200);           /* et le calcul ne la reprend pas */
    CHECK_NEAR(get(c, N2K_SET, 60200), 200 * D, 1e-3);
    CHECK_NEAR(get(c, N2K_DRIFT, 60200), 0.30, 1e-9);
    n2k_get_info(c, N2K_DRIFT, 60200, &src, NULL, NULL);
    CHECK(src == 5);
    n2k_destroy(c);

    /* Capture sim complète avec CM50 : le simulateur produit sa BSP depuis la
     * même polaire → % polaire ≈ 100. Cohérence TWD / cibles. */
    char path[512], err[128];
    size_t len;
    char *txt;
    snprintf(path, sizeof path, "%s/CM50.pol", dir);
    if (!(txt = slurp(path, &len)))
        return;
    n2k_polar *p = n2k_polar_parse(txt, len, err, sizeof err);
    snprintf(path, sizeof path, "%s/sim-30s.ydraw", dir);
    FILE *f = fopen(path, "r");
    if (!p || !f) { fails++; if (f) fclose(f); n2k_polar_free(p); return; }

    n2k_ctx *full = n2k_create(), *nocur = n2k_create();
    n2k_set_polar(full, p);
    char line[256];
    uint64_t now = 0;
    while (fgets(line, sizeof line, f)) {
        uint32_t tod, id;
        uint8_t d[8];
        int l;
        if (n2k_parse_ydraw(line, &tod, &id, d, &l))
            now = tod;
        n2k_feed_line(full, line, now);
        if (!strstr(line, " R 09F90B") && !strstr(line, " R 0DF90B"))   /* sans 129291 */
            n2k_feed_line(nocur, line, now);
    }
    fclose(f);

    double hdg = get(full, N2K_HDG_TRUE, now), twa = get(full, N2K_TWA, now);
    CHECK_NEAR(get(full, N2K_TWD, now), wrapd(hdg + twa), 1e-3);
    CHECK_NEAR(get(full, N2K_POLAR_PCT, now), 100.0, 0.5);
    double t_twa = get(full, N2K_TARGET_TWA, now);
    CHECK(t_twa >= get(full, N2K_VMG_RANGE_MIN, now) - 1e-9);
    CHECK(t_twa <= get(full, N2K_VMG_RANGE_MAX, now) + 1e-9);
    CHECK(get(full, N2K_VMG_PCT, now) > 0 && get(full, N2K_VMG_PCT, now) < 100);
    n2k_get_info(full, N2K_SET, now, &src, NULL, NULL);
    CHECK(src == 5);                                   /* 129291 du sim prioritaire */

    /* Courant calculé ≈ courant du simulateur. */
    n2k_get_info(nocur, N2K_SET, now, &src, NULL, NULL);
    CHECK(src == N2K_SRC_CALC);
    CHECK_NEAR(get(nocur, N2K_DRIFT, now), get(full, N2K_DRIFT, now), 0.03);
    CHECK_NEAR(get(nocur, N2K_SET, now), get(full, N2K_SET, now), 3 * D);

    n2k_destroy(full);
    n2k_destroy(nocur);
    n2k_polar_free(p);
}

/* Capture sim aléatoire (vent et courant variables) : courant calculé avec une
 * fenêtre de 1 s comparé au 129291 du simulateur toutes les 10 s. */
static void test_current_alea(const char *dir)
{
    char path[512];
    snprintf(path, sizeof path, "%s/sim-alea-180s.ydraw", dir);
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "impossible d'ouvrir %s\n", path); fails++; return; }
    n2k_ctx *ref = n2k_create(), *calc = n2k_create();
    n2k_set_windows(calc, 0, 0, 1);
    char line[256];
    uint64_t now = 0, next = 0;
    int n = 0;
    double worst = 0;
    while (fgets(line, sizeof line, f)) {
        uint32_t tod, id;
        uint8_t d[8];
        int l;
        if (n2k_parse_ydraw(line, &tod, &id, d, &l))
            now = tod;
        n2k_feed_line(ref, line, now);
        if (!strstr(line, " R 0DF90B") && !strstr(line, " R 09F90B"))
            n2k_feed_line(calc, line, now);
        if (next == 0) next = now + 10000;
        if (now >= next) {
            double a = get(ref, N2K_DRIFT, now), b = get(calc, N2K_DRIFT, now);
            if (fabs(a - b) > worst) worst = fabs(a - b);
            CHECK_NEAR(b, a, 0.015);            /* m/s, ≈ 0,03 kn */
            int src;
            n2k_get_info(calc, N2K_DRIFT, now, &src, NULL, NULL);
            CHECK(src == N2K_SRC_CALC);
            n++;
            next += 10000;
        }
    }
    fclose(f);
    CHECK(n >= 15);
    fprintf(stderr, "courant aléatoire : %d points, écart max %.3f kn\n", n, worst * 3600 / 1852);
    n2k_destroy(ref);
    n2k_destroy(calc);
}

/* Capture sim au près, TWS 14,7–16,9 kn autour de la zone bimodale de CM50 :
 * l'optimum brut sur TWS moyennée bascule 4 fois, l'angle stabilisé 2 fois
 * seulement (seuils 16,57 / 14,89 kn de TWS moyennée). */
static void test_hyst_capture(const char *dir)
{
    const double D = 3.14159265358979323846 / 180.0;
    char path[512], err[128];
    size_t len;
    char *txt;
    snprintf(path, sizeof path, "%s/CM50.pol", dir);
    if (!(txt = slurp(path, &len)))
        return;
    n2k_polar *p = n2k_polar_parse(txt, len, err, sizeof err);
    snprintf(path, sizeof path, "%s/sim-hyst-900s.ydraw", dir);
    FILE *f = fopen(path, "r");
    if (!p || !f) { fails++; if (f) fclose(f); n2k_polar_free(p); return; }
    n2k_ctx *c = n2k_create();
    n2k_set_polar(c, p);
    char line[256];
    uint64_t now = 0, next = 0;
    double prev = -1;
    int sw = 0;
    while (fgets(line, sizeof line, f)) {
        uint32_t tod, id;
        uint8_t d[8];
        int l;
        if (n2k_parse_ydraw(line, &tod, &id, d, &l))
            now = tod;
        n2k_feed_line(c, line, now);
        if (next == 0) next = now + 1000;
        if (now < next) continue;
        next += 1000;
        double ts;
        if (n2k_get(c, N2K_TARGET_TWA, now, &ts) != N2K_OK)
            continue;
        if (prev >= 0 && fabs(ts - prev) > 3 * D)
            sw++;
        prev = ts;
    }
    fclose(f);
    CHECK(sw == 2);
    n2k_destroy(c);
    n2k_polar_free(p);
}

/* Laylines : formules, historique autour du Nord, cohérence sur la capture au près. */
static void test_laylines(const char *dir)
{
    const double D = 3.14159265358979323846 / 180.0;
    char path[512], err[128];
    size_t len;
    char *txt;
    snprintf(path, sizeof path, "%s/CM50.pol", dir);
    if (!(txt = slurp(path, &len)))
        return;
    n2k_polar *p = n2k_polar_parse(txt, len, err, sizeof err);
    if (!p) { fails++; return; }

    /* Vent qui oscille autour du Nord : TWA fixe 40° tribord, cap alternant
     * 310°/330° chaque seconde → TWD alternant 350°/10°. */
    n2k_ctx *c = n2k_create();
    n2k_set_polar(c, p);
    uint64_t t = 0;
    for (; t < 120000; t += 250) {
        heading(c, ((t / 1000) % 2 ? 310 : 330) * D, t);
        speed(c, 6.0, t);
        wind(c, 40 * D, 6.0, 4, t);           /* 11,7 kn de TWS, tribord amures */
    }
    double opt = get(c, N2K_TARGET_TWA, t - 250);
    double twd = get(c, N2K_TWD, t - 250);
    CHECK(opt > 30 * D && opt < 50 * D);
    CHECK_NEAR(get(c, N2K_LAY_STBD, t - 250), wrapd(twd - opt), 1e-9);
    CHECK_NEAR(get(c, N2K_LAY_PORT, t - 250), wrapd(twd + opt), 1e-9);
    /* Historique : 350° → 10° en passant par le Nord (secteur de 20°, pas 340°). */
    double lo = get(c, N2K_TWD_MIN, t - 250), hi = get(c, N2K_TWD_MAX, t - 250);
    CHECK_NEAR(lo, 350 * D, 0.2 * D);
    CHECK_NEAR(hi, 10 * D, 0.2 * D);
    CHECK_NEAR(get(c, N2K_LAY_STBD_MIN, t - 250), wrapd(350 * D - opt), 0.2 * D);
    CHECK_NEAR(get(c, N2K_LAY_PORT_MAX, t - 250), wrapd(10 * D + opt), 0.2 * D);
    n2k_destroy(c);

    /* Capture au près : TWD dans [MIN, MAX] (sens horaire), secteur ≈ ±5°. */
    snprintf(path, sizeof path, "%s/sim-hyst-900s.ydraw", dir);
    FILE *f = fopen(path, "r");
    if (!f) { fails++; n2k_polar_free(p); return; }
    c = n2k_create();
    n2k_set_polar(c, p);
    char line[256];
    uint64_t now = 0, next = 0;
    int checks_done = 0;
    while (fgets(line, sizeof line, f)) {
        uint32_t tod, id;
        uint8_t d[8];
        int l;
        if (n2k_parse_ydraw(line, &tod, &id, d, &l))
            now = tod;
        n2k_feed_line(c, line, now);
        if (next == 0) next = now + 30000;
        if (now < next) continue;
        next += 30000;
        double w = get(c, N2K_TWD, now), a = get(c, N2K_TWD_MIN, now), b = get(c, N2K_TWD_MAX, now);
        double span = wrapd(b - a), pos = wrapd(w - a);
        /* TWD instantané à ±1° du secteur (le secteur est fait de moyennes par seconde). */
        CHECK(pos <= span + 1 * D || pos >= 2 * 3.14159265358979323846 - 1 * D);
        CHECK(span < 15 * D);
        checks_done++;
    }
    fclose(f);
    CHECK(checks_done >= 25);
    n2k_destroy(c);
    n2k_polar_free(p);
}

/* Laylines fond. Un bateau qui navigue exactement sur sa layline tribord, à la
 * vitesse cible, dans un courant connu, doit avoir une COG = LAY_STBD_GND, que le
 * courant soit fourni (129291) ou calculé (SOG/COG − BSP/cap, sans dérive ici). */
static double wrap_pi_t(double a)
{
    a = wrapd(a);
    return a > 3.14159265358979323846 ? a - 2 * 3.14159265358979323846 : a;
}

static void test_laylines_ground(const char *dir)
{
    const double D = 3.14159265358979323846 / 180.0;
    char path[512], err[128];
    size_t len;
    char *txt;
    snprintf(path, sizeof path, "%s/CM50.pol", dir);
    if (!(txt = slurp(path, &len)))
        return;
    n2k_polar *p = n2k_polar_parse(txt, len, err, sizeof err);
    if (!p) { fails++; return; }

    for (int mode = 0; mode < 2; mode++) {        /* 0 : 129291 fourni, 1 : calculé */
        n2k_ctx *c = n2k_create();
        n2k_set_polar(c, p);
        const double twd = 20 * D, tws = 6.0, ce = 1.0 * sin(300 * D), cn = 1.0 * cos(300 * D);
        /* Phase 1 : obtenir l'angle optimum et la vitesse cible pour cette TWS. */
        uint64_t t = 0;
        for (; t < 70000; t += 250) {
            heading(c, wrapd(twd - 40 * D), t);
            speed(c, 4.0, t);
            wind(c, 40 * D, tws, 4, t);
        }
        double opt = get(c, N2K_TARGET_TWA, t - 250), tb = get(c, N2K_TARGET_BSP, t - 250);
        CHECK(isfinite(opt) && isfinite(tb));
        /* Phase 2 : sur la layline tribord, à la vitesse cible. */
        double h = wrapd(twd - opt);
        double ge = tb * sin(h) + ce, gn = tb * cos(h) + cn;
        double cog = wrapd(atan2(ge, gn)), sog = sqrt(ge * ge + gn * gn);
        for (uint64_t end = t + 120000; t < end; t += 250) {
            heading(c, h, t);
            speed(c, tb, t);
            wind(c, opt, tws, 4, t);
            if (mode == 0)
                setdrift(c, 300 * D, 1.0, t);
            cogsog(c, cog, sog, t);
        }
        uint64_t now = t - 250;
        int src;
        n2k_get_info(c, N2K_SET, now, &src, NULL, NULL);
        CHECK(mode == 0 ? src == 5 : src == N2K_SRC_CALC);
        CHECK_NEAR(get(c, N2K_LAY_STBD, now), h, 0.1 * D);
        CHECK_NEAR(get(c, N2K_LAY_STBD_GND, now), cog, 0.3 * D);
        /* Le courant (vers 300°) déporte la layline fond de la bonne valeur. */
        double off = wrapd(get(c, N2K_LAY_STBD_GND, now) - h);
        if (off > 3.14159265358979323846) off -= 2 * 3.14159265358979323846;
        CHECK(fabs(off) > 2 * D);
        n2k_destroy(c);
    }

    /* Courant nul (instrument) et pas de dérive (COG = cap, SOG = BSP) :
     * dérive mesurée nulle → laylines fond = laylines au cap. */
    n2k_ctx *c = n2k_create();
    n2k_set_polar(c, p);
    uint64_t t = 0;
    for (; t < 120000; t += 250) {
        heading(c, 0, t);
        speed(c, 4.0, t);
        wind(c, 40 * D, 6.0, 4, t);
        setdrift(c, 0, 0.0, t);
        cogsog(c, 0, 4.0, t);
    }
    CHECK(n2k_get(c, N2K_LEEWAY, t - 250, NULL) == N2K_OK);
    CHECK_NEAR(get(c, N2K_LEEWAY, t - 250), 0, 0.05 * D);
    CHECK_NEAR(get(c, N2K_LAY_STBD_GND, t - 250), get(c, N2K_LAY_STBD, t - 250), 1e-9);
    CHECK_NEAR(get(c, N2K_LAY_PORT_GND, t - 250), get(c, N2K_LAY_PORT, t - 250), 1e-9);
    /* Sans courant ni dérive : amure opposée au même TWA = TWD + |TWA| (tribord → bâbord). */
    double twa = get(c, N2K_TWA, t - 250);
    CHECK(twa > 0);
    CHECK_NEAR(wrap_pi_t(get(c, N2K_OPP_TWA_GND, t - 250) - (get(c, N2K_TWD, t - 250) + twa)), 0, 1e-6);
    n2k_destroy(c);
    n2k_polar_free(p);
}

/* x dans la bande [a, b] (bords dans un ordre quelconque, < 180°) ? */
static int in_band(double x, double a, double b, double eps)
{
    double span = wrap_pi_t(b - a), pos = wrap_pi_t(x - a);
    return span >= 0 ? (pos >= -eps && pos <= span + eps) : (pos <= eps && pos >= span - eps);
}

/* Bateau simulé : dérive λ = 5°, courant 0,8 m/s vers 300°, sans 129291.
 * Bord tribord, virement, bord bâbord, puis 40 min sans revirer. */
static void test_leeway(const char *dir)
{
    const double D = 3.14159265358979323846 / 180.0;
    char path[512], err[128];
    size_t len;
    char *txt;
    snprintf(path, sizeof path, "%s/CM50.pol", dir);
    if (!(txt = slurp(path, &len)))
        return;
    n2k_polar *p = n2k_polar_parse(txt, len, err, sizeof err);
    if (!p) { fails++; return; }

    const double lam = 5 * D, twd = 20 * D, tws = 6.0;
    const double ce = 0.8 * sin(300 * D), cn = 0.8 * cos(300 * D);
    double opt, tb;
    CHECK(n2k_polar_target(p, tws, 1, &opt, &tb));
    const double hs = wrapd(twd - opt), hp = wrapd(twd + opt);
    /* Laylines fond exactes (vitesse cible, dérive et courant réels). */
    const double true_s = wrapd(atan2(tb * sin(hs - lam) + ce, tb * cos(hs - lam) + cn));
    const double true_p = wrapd(atan2(tb * sin(hp + lam) + ce, tb * cos(hp + lam) + cn));

    n2k_ctx *c = n2k_create();
    n2k_set_polar(c, p);
    uint64_t t = 0;

#define SAIL(stbd, dur_ms) \
    for (uint64_t end_ = t + (dur_ms); t < end_; t += 250) { \
        double h_ = (stbd) ? hs : hp, tr_ = (stbd) ? h_ - lam : h_ + lam; \
        double ge_ = tb * sin(tr_) + ce, gn_ = tb * cos(tr_) + cn; \
        heading(c, h_, t); speed(c, tb, t); \
        wind(c, (stbd) ? opt : -opt, tws, 4, t); \
        cogsog(c, wrapd(atan2(ge_, gn_)), sqrt(ge_ * ge_ + gn_ * gn_), t); }

    /* 1. Bord tribord seul : amure courante juste, bord opposé incertain. */
    SAIL(1, 150000);
    uint64_t now = t - 250;
    CHECK(n2k_get(c, N2K_LEEWAY, now, NULL) == N2K_NEVER);
    CHECK_NEAR(get(c, N2K_LEEWAY_SIGMA, now), 4 * D, 1e-9);          /* a priori */
    CHECK_NEAR(wrap_pi_t(get(c, N2K_LAY_STBD_GND, now) - true_s), 0, 0.5 * D);
    double w_cur = fabs(wrap_pi_t(get(c, N2K_LAY_STBD_GND_HI, now) - get(c, N2K_LAY_STBD_GND_LO, now)));
    double w_opp = fabs(wrap_pi_t(get(c, N2K_LAY_PORT_GND_HI, now) - get(c, N2K_LAY_PORT_GND_LO, now)));
    fprintf(stderr, "dérive, un seul bord : bande amure courante %.2f°, opposée %.2f°\n", w_cur / D, w_opp / D);
    CHECK(w_cur < 1.0 * D);
    CHECK(w_opp > 5.0 * D);
    CHECK(in_band(true_p, get(c, N2K_LAY_PORT_GND_LO, now), get(c, N2K_LAY_PORT_GND_HI, now), 0.2 * D));

    /* 2. Virement, bord bâbord : λ mesuré, les deux laylines justes. */
    SAIL(0, 150000);
    now = t - 250;
    CHECK(n2k_get(c, N2K_LEEWAY, now, NULL) == N2K_OK);
    CHECK_NEAR(get(c, N2K_LEEWAY, now), lam, 0.3 * D);
    CHECK(get(c, N2K_LEEWAY_SIGMA, now) < 1.5 * D);
    CHECK_NEAR(wrap_pi_t(get(c, N2K_LAY_PORT_GND, now) - true_p), 0, 0.5 * D);
    CHECK_NEAR(wrap_pi_t(get(c, N2K_LAY_STBD_GND, now) - true_s), 0, 0.5 * D);
    w_opp = fabs(wrap_pi_t(get(c, N2K_LAY_STBD_GND_HI, now) - get(c, N2K_LAY_STBD_GND_LO, now)));
    fprintf(stderr, "dérive mesurée %.2f° (vraie 5°), bande opposée %.2f°\n",
            get(c, N2K_LEEWAY, now) / D, w_opp / D);
    CHECK(w_opp < 3.0 * D);

    /* 3. 40 min sans revirer : l'incertitude revient à l'a priori. */
    SAIL(0, 40 * 60000);
    now = t - 250;
    CHECK_NEAR(get(c, N2K_LEEWAY_SIGMA, now), 4 * D, 1e-6);
    CHECK_NEAR(get(c, N2K_LEEWAY, now), lam, 0.3 * D);             /* centre conservé */
    w_opp = fabs(wrap_pi_t(get(c, N2K_LAY_STBD_GND_HI, now) - get(c, N2K_LAY_STBD_GND_LO, now)));
    CHECK(w_opp > 5.0 * D);
    CHECK(in_band(true_s, get(c, N2K_LAY_STBD_GND_LO, now), get(c, N2K_LAY_STBD_GND_HI, now), 0.2 * D));
#undef SAIL

    n2k_destroy(c);
    n2k_polar_free(p);
}

/* TWA_VMG_PCT : 100 à l'angle optimum, ≈ 99 aux bords de la plage, bas au travers. */
static void test_twa_vmg_pct(const char *dir)
{
    const double D = 3.14159265358979323846 / 180.0;
    char path[512], err[128];
    size_t len;
    char *txt;
    snprintf(path, sizeof path, "%s/CM50.pol", dir);
    if (!(txt = slurp(path, &len)))
        return;
    n2k_polar *p = n2k_polar_parse(txt, len, err, sizeof err);
    if (!p) { fails++; return; }
    const double tws = 5.0;                      /* ≈ 9,7 kn, hors zone bimodale */
    double opt, tb, lo, hi;
    n2k_polar_target(p, tws, 1, &opt, &tb);
    n2k_polar_vmg_range(p, tws, 1, 0.99, &lo, &hi);
    double twas[4] = { opt, lo, hi, 85 * D };
    double pct[4];
    for (int k = 0; k < 4; k++) {
        n2k_ctx *c = n2k_create();
        n2k_set_polar(c, p);
        heading(c, 0, 0);
        speed(c, 4.0, 0);
        wind(c, twas[k], tws, 4, 0);
        pct[k] = get(c, N2K_TWA_VMG_PCT, 0);
        n2k_destroy(c);
    }
    CHECK_NEAR(pct[0], 100.0, 0.05);
    CHECK_NEAR(pct[1], 99.0, 0.2);
    CHECK_NEAR(pct[2], 99.0, 0.2);
    CHECK(pct[3] < 60);
    /* Bâbord : symétrique. */
    n2k_ctx *c = n2k_create();
    n2k_set_polar(c, p);
    heading(c, 0, 0); speed(c, 4.0, 0); wind(c, -opt, tws, 4, 0);
    CHECK_NEAR(get(c, N2K_TWA_VMG_PCT, 0), 100.0, 0.05);
    n2k_destroy(c);
    n2k_polar_free(p);
}

/* 129284 Navigation Data (fast-packet, 34 octets) : distance et relèvement vrai. */
/* 129284 ; wcv < 0 : vitesse de rapprochement absente. */
static void navdata_wcv(n2k_ctx *c, double dtw, double btw, double wcv, uint64_t t)
{
    static int seq = 0;
    uint8_t m[34];
    memset(m, 0xFF, sizeof m);
    if (wcv >= 0) put16(m + 32, (unsigned)lround(wcv / 0.01));
    uint32_t d = (uint32_t)lround(dtw / 0.01);
    m[1] = (uint8_t)d; m[2] = (uint8_t)(d >> 8); m[3] = (uint8_t)(d >> 16); m[4] = (uint8_t)(d >> 24);
    m[5] = 0xF0;                                         /* référence vraie */
    put16(m + 14, (unsigned)lround(wrapd(btw) / 1e-4));
    uint32_t id = (3u << 26) | (0x1F904u << 8) | 7u;
    uint8_t f[8];
    int got = 0, idx = 0;
    while (got < 34) {
        memset(f, 0xFF, sizeof f);
        if (idx == 0) {
            f[0] = (uint8_t)(seq << 5); f[1] = 34;
            memcpy(f + 2, m, 6); got = 6;
        } else {
            f[0] = (uint8_t)((seq << 5) | idx);
            int n = 34 - got < 7 ? 34 - got : 7;
            memcpy(f + 1, m + got, (size_t)n); got += n;
        }
        n2k_feed_frame(c, id, f, 8, t);
        idx++;
    }
    seq = (seq + 1) & 7;
}

static void navdata(n2k_ctx *c, double dtw, double btw, uint64_t t)
{
    navdata_wcv(c, dtw, btw, -1, t);
}

/* Temps et distances jusqu'au WP en tirant des bords sur les laylines fond. */
static void test_route_wp(const char *dir)
{
    const double D = 3.14159265358979323846 / 180.0;
    char path[512], err[128];
    size_t len;
    char *txt;
    snprintf(path, sizeof path, "%s/CM50.pol", dir);
    if (!(txt = slurp(path, &len)))
        return;
    n2k_polar *p = n2k_polar_parse(txt, len, err, sizeof err);
    if (!p) { fails++; return; }
    const double tws = 6.0;
    double opt, tb;
    CHECK(n2k_polar_target(p, tws, 1, &opt, &tb));

    /* TWD 0°, courant nul (instrument), dérive a priori nulle. */
    struct { double btw, dtw; int ok; double ta, tbp; } cas[3] = {
        { 0, 2000, 1, 0, 0 },                  /* plein vent : deux bords égaux */
        { 0, 2000, 1, 0, 0 },                  /* rempli plus bas : WP sur la layline tribord */
        { 180 * D, 2000, 0, 0, 0 },            /* derrière (au portant) : rien */
    };
    cas[1].btw = wrapd(-opt);
    for (int k = 0; k < 3; k++) {
        n2k_ctx *c = n2k_create();
        n2k_set_polar(c, p);
        n2k_set_leeway_prior(c, 0, 0, 0, 0);
        heading(c, wrapd(-opt), 0);
        speed(c, tb, 0);
        setdrift(c, 0, 0.0, 0);
        navdata(c, cas[k].dtw, cas[k].btw, 0);
        wind(c, opt, tws, 4, 0);
        if (!cas[k].ok) {
            CHECK(n2k_get(c, N2K_TTW, 0, NULL) == N2K_NEVER);
        } else if (k == 0) {
            double leg = 2000 / (2 * cos(opt));
            CHECK_NEAR(get(c, N2K_DTW_SAIL, 0), 2 * leg, 2.0);
            CHECK_NEAR(get(c, N2K_TTW_STBD, 0), leg / tb, 1.0);
            CHECK_NEAR(get(c, N2K_TTW_PORT, 0), leg / tb, 1.0);
            CHECK_NEAR(get(c, N2K_TTW, 0), 2 * leg / tb, 2.0);
        } else {
            CHECK_NEAR(get(c, N2K_TTW_STBD, 0), 2000 / tb, 1.0);
            CHECK_NEAR(get(c, N2K_TTW_PORT, 0), 0, 1.0);
            CHECK_NEAR(get(c, N2K_DTW_SAIL, 0), 2000, 2.0);
        }
        n2k_destroy(c);
    }

    /* WP juste au-delà de la layline tribord (sur la layline, à 1° près) et
     * par le travers tribord : faisable directement tribord amures. */
    for (int k = 0; k < 2; k++) {
        n2k_ctx *c = n2k_create();
        n2k_set_polar(c, p);
        n2k_set_leeway_prior(c, 0, 0, 0, 0);
        heading(c, wrapd(-opt), 0); speed(c, tb, 0);
        setdrift(c, 0, 0.0, 0);
        navdata(c, 2000, k == 0 ? wrapd(-opt - 1 * D) : 270 * D, 0);
        wind(c, opt, tws, 4, 0);
        CHECK_NEAR(get(c, N2K_TTW_STBD, 0), 2000 / tb, 1.0);
        CHECK_NEAR(get(c, N2K_TTW_PORT, 0), 0, 1e-9);
        CHECK_NEAR(get(c, N2K_DTW_SAIL, 0), 2000, 1e-6);
        n2k_destroy(c);
    }

    /* Faisable directement avec une VMC reçue (WCV) : temps = DTW / VMC, comme qtVlm. */
    {
        n2k_ctx *c = n2k_create();
        n2k_set_polar(c, p);
        n2k_set_leeway_prior(c, 0, 0, 0, 0);
        heading(c, wrapd(-opt), 0); speed(c, tb, 0);
        setdrift(c, 0, 0.0, 0);
        navdata_wcv(c, 2000, 270 * D, 5.0, 0);
        wind(c, opt, tws, 4, 0);
        CHECK_NEAR(get(c, N2K_TTW_STBD, 0), 2000 / 5.0, 1.0);
        CHECK_NEAR(get(c, N2K_TTW, 0), 2000 / 5.0, 1.0);
        n2k_destroy(c);
    }

    /* Avec 1 m/s de courant portant vers l'Est : le bord bâbord (vers le NE)
     * est plus rapide sur le fond, le tribord (vers le NW) plus lent. */
    n2k_ctx *c = n2k_create();
    n2k_set_polar(c, p);
    n2k_set_leeway_prior(c, 0, 0, 0, 0);
    heading(c, wrapd(-opt), 0); speed(c, tb, 0);
    setdrift(c, 90 * D, 1.0, 0);
    navdata(c, 2000, 0, 0);
    wind(c, opt, tws, 4, 0);
    CHECK(get(c, N2K_TTW_PORT, 0) > 0 && get(c, N2K_TTW_STBD, 0) > get(c, N2K_TTW_PORT, 0));
    n2k_destroy(c);
    n2k_polar_free(p);
}

/* Flux de navigation de qtVlm (format passerelle, sans horodatage), trames
 * réelles relevées en UDP 2800 : WP à 74 921,77 m au 187,9° vrai. */
static void test_qtvlm_nav(void)
{
    static const char *const l[] = {
        "09F11244 FF FE 77 FF 7F FF 7F FC\r\n",     /* cap qtVlm : à ignorer */
        "09FD0244 FF AA 04 D1 15 FA FF FF\r\n",     /* vent qtVlm : à ignorer */
        "0DF90444 00 22 FF 51 52 72 00 00\r\n",
        "0DF90444 01 FF FF FF FF FF FF FF\r\n",
        "0DF90444 02 FF 21 80 00 00 00 00\r\n",
        "0DF90444 03 01 00 00 00 58 09 74\r\n",
        "0DF90444 04 1B 51 F2 54 FE F6 01\r\n",
    };
    const double D = 3.14159265358979323846 / 180.0;
    uint32_t tod, id;
    uint8_t d[8];
    int len;
    CHECK(n2k_parse_ydraw(l[2], &tod, &id, d, &len) && id == 0x0DF90444u && len == 8 && tod == 0);
    CHECK(!n2k_parse_ydraw("0DF90444 0", &tod, &id, d, &len));
    CHECK(!n2k_parse_ydraw("ZZ", &tod, &id, d, &len));

    /* Navigation seulement, chaque datagramme reçu trois fois (trois interfaces). */
    n2k_ctx *c = n2k_create();
    for (int i = 0; i < 7; i++)
        for (int r = 0; r < 3; r++)
            CHECK(n2k_feed_line_ex(c, l[i], 100, N2K_FEED_NAV_ONLY));
    CHECK_NEAR(get(c, N2K_DTW, 100), 74921.77, 1e-6);
    CHECK_NEAR(get(c, N2K_BTW, 100), 0x8021 * 1e-4, 1e-9);
    CHECK_NEAR(get(c, N2K_BTW, 100) / D, 187.9, 0.05);
    CHECK_NEAR(get(c, N2K_WCV, 100), 5.02, 1e-9);
    CHECK(n2k_get(c, N2K_DTW, 100 + 9000, NULL) == N2K_OK);              /* nav : 10 s */
    CHECK(n2k_get(c, N2K_DTW, 100 + 10001, NULL) == N2K_STALE);
    CHECK(n2k_get(c, N2K_HDG_TRUE, 100, NULL) == N2K_NEVER);   /* filtré */
    CHECK(n2k_get(c, N2K_AWA, 100, NULL) == N2K_NEVER);
    n2k_stats s;
    n2k_get_stats(c, &s);
    CHECK(s.fp_complete == 1 && s.fp_dropped == 0);
    n2k_destroy(c);

    /* Sans filtre, le cap de qtVlm serait pris : d'où le filtre sur ce flux. */
    c = n2k_create();
    n2k_feed_line(c, l[0], 0);
    CHECK(n2k_get(c, N2K_HDG_TRUE, 0, NULL) == N2K_OK);
    n2k_destroy(c);
}

/* Profil de VMG : 100 à l'angle optimum de chaque allure, bas au travers. */
static void test_vmg_profile(const char *dir)
{
    char path[512], err[128];
    size_t len;
    char *txt;
    snprintf(path, sizeof path, "%s/CM50.pol", dir);
    if (!(txt = slurp(path, &len)))
        return;
    n2k_polar *p = n2k_polar_parse(txt, len, err, sizeof err);
    if (!p) { fails++; return; }
    double pct[181];
    n2k_ctx *c = n2k_create();
    CHECK(!n2k_vmg_profile(c, pct));                     /* ni polaire ni vent */
    n2k_set_polar(c, p);
    heading(c, 0, 0); speed(c, 4.0, 0); wind(c, 0.7, 5.0, 4, 0);
    CHECK(n2k_vmg_profile(c, pct));
    double up, dn, vu, vd;
    n2k_polar_target(p, 5.0, 1, &up, &vu);
    n2k_polar_target(p, 5.0, 0, &dn, &vd);
    int ku = (int)lround(up * 180 / 3.14159265358979323846);
    int kd = (int)lround(dn * 180 / 3.14159265358979323846);
    CHECK(pct[ku] > 99.5 && pct[kd] > 99.5);
    CHECK(pct[0] == 0 && pct[90] < 30 && pct[180] < 100);
    for (int k = 0; k <= 180; k++)
        CHECK(pct[k] >= 0 && pct[k] <= 100);
    n2k_destroy(c);
    n2k_polar_free(p);
}

/* 129285 réel de qtVlm : route « qtVlm », WP n° 1 « WP Sud » ; accepté par le
 * filtre navigation ; nom gardé pour le WP de destination de 129284. */
static void test_wp_name(void)
{
    static const char *const l[] = {
        "1DF90544 00 23 00 00 01 00 00 00",
        "1DF90544 01 00 00 E0 07 01 71 74",
        "1DF90544 02 56 6C 6D FF 01 00 08",
        "1DF90544 03 01 57 50 20 53 75 64",
        "1DF90544 04 83 05 EA 1B 1D 42 41",
        "1DF90544 05 FE FF FF FF FF FF FF",
    };
    char name[48];
    n2k_ctx *c = n2k_create();
    CHECK(n2k_wp_name(c, 0, name, sizeof name) == N2K_NEVER && name[0] == 0);
    for (int i = 0; i < 6; i++)
        CHECK(n2k_feed_line_ex(c, l[i], 1000, N2K_FEED_NAV_ONLY));
    CHECK(n2k_wp_name(c, 1000, name, sizeof name) == N2K_OK);
    CHECK(strcmp(name, "WP Sud") == 0);
    CHECK(n2k_wp_name(c, 1000 + 5000, name, sizeof name) == N2K_OK);     /* nav : 10 s */
    CHECK(n2k_wp_name(c, 1000 + 10001, name, sizeof name) == N2K_STALE);
    /* Tampon court : tronqué proprement. */
    char small[4];
    n2k_wp_name(c, 1000, small, sizeof small);
    CHECK(strcmp(small, "WP ") == 0);
    n2k_destroy(c);
}

/* Nom du WP accentué (plantage JNI constaté : octet Latin-1 transmis tel quel).
 * La sortie doit toujours être de l'UTF-8 valide. */
static void wp_name_with(const char *frame3, const char *expect, size_t bufsize)
{
    const char *l[] = {
        "1DF90544 00 23 00 00 01 00 00 00",
        "1DF90544 01 00 00 E0 07 01 71 74",
        "1DF90544 02 56 6C 6D FF 01 00 08",
        frame3,
        "1DF90544 04 83 05 EA 1B 1D 42 41",
        "1DF90544 05 FE FF FF FF FF FF FF",
    };
    char name[48];
    n2k_ctx *c = n2k_create();
    for (int i = 0; i < 6; i++)
        n2k_feed_line_ex(c, l[i], 1000, N2K_FEED_NAV_ONLY);
    CHECK(n2k_wp_name(c, 1000, name, bufsize) == N2K_OK);
    CHECK(strcmp(name, expect) == 0);
    n2k_destroy(c);
}

static void test_wp_name_accents(void)
{
    wp_name_with("1DF90544 03 01 46 6C E8 63 68 65", "Fl\xC3\xA8" "che", 48);  /* Latin-1 « Flèche » */
    wp_name_with("1DF90544 03 01 43 61 70 C3 A9 65", "Cap\xC3\xA9" "e", 48);   /* UTF-8 « Capée » */
    wp_name_with("1DF90544 03 01 41 C3 72 64 65 6E", "A\xC3\x83" "rden", 48);  /* UTF-8 invalide → Latin-1 */
    wp_name_with("1DF90544 03 01 43 61 70 C3 A9 65", "Cap", 5);                  /* pas de « é » coupé */
}

/* Envoie un message fast-packet (trames de 8 octets, séquence 0). */
static void feed_fast(n2k_ctx *c, uint32_t id, const uint8_t *m, int n, uint64_t t)
{
    uint8_t f[8];
    int o = 0;
    for (int k = 0; o < n; k++) {
        memset(f, 0xFF, 8);
        f[0] = (uint8_t)k;
        int start = 1;
        if (k == 0) { f[1] = (uint8_t)n; start = 2; }
        for (int i = start; i < 8 && o < n; i++)
            f[i] = m[o++];
        n2k_feed_frame(c, id, f, 8, t);
    }
}

static void put32(uint8_t *d, uint32_t v)
{
    d[0] = (uint8_t)v; d[1] = (uint8_t)(v >> 8); d[2] = (uint8_t)(v >> 16); d[3] = (uint8_t)(v >> 24);
}

/* Rapport de position AIS (129038 si status ≥ 0, sinon 129039). */
static void ais_pos(n2k_ctx *c, uint32_t mmsi, double lat, double lon, double cog_deg,
                    double sog, int status, uint64_t t)
{
    uint8_t m[28];
    memset(m, 0xFF, sizeof m);
    m[0] = 1;
    put32(m + 1, mmsi);
    put32(m + 5, (uint32_t)(int32_t)lround(lon * 1e7));
    put32(m + 9, (uint32_t)(int32_t)lround(lat * 1e7));
    unsigned cog = (unsigned)lround(cog_deg * 3.14159265358979323846 / 180.0 * 1e4);
    unsigned sg = (unsigned)lround(sog * 100);
    m[14] = (uint8_t)cog; m[15] = (uint8_t)(cog >> 8);
    m[16] = (uint8_t)sg;  m[17] = (uint8_t)(sg >> 8);
    if (status >= 0) {
        m[25] = (uint8_t)(0xF0 | status);
        feed_fast(c, 0x11F80EB2u, m, 28, t);             /* 129038, src 178 */
    } else {
        feed_fast(c, 0x11F80FB2u, m, 27, t);             /* 129039 */
    }
}

/* AIS : option « cible immobile jamais dangereuse ». Notre bateau à 5 m/s vers
 * le Nord, bateau au mouillage (0 m/s) 1 NM droit devant : dangereux par défaut,
 * suivi seulement (niveau 1) avec l'option ; une cible en route reste dangereuse. */
static void test_ais_still(void)
{
    n2k_ctx *c = n2k_create();
    n2k_ais_target t;
    uint64_t now = 10000;
    const double lat0 = 47.0, lon0 = -3.0, nm_lat = 1.0 / 60.0;
    uint8_t pos[8], cs[8] = { 0xFF, 0xFC, 0, 0, 0xF4, 0x01, 0xFF, 0xFF };
    put32(pos, (uint32_t)(int32_t)lround(lat0 * 1e7));
    put32(pos + 4, (uint32_t)(int32_t)lround(lon0 * 1e7));
    n2k_feed_frame(c, 0x09F80102u, pos, 8, now);
    n2k_feed_frame(c, 0x09F80202u, cs, 8, now);
    ais_pos(c, 227000010, lat0 + nm_lat, lon0, 0, 0, 1, now);
    CHECK(n2k_ais_threat(c, now, 926, 1200, 0, NULL, 0, &t) == 2 && t.mmsi == 227000010);
    n2k_set_ais_ignore_still(c, 1);
    CHECK(n2k_ais_threat(c, now, 926, 1200, 0, NULL, 0, &t) == 1 && t.mmsi == 227000010);
    ais_pos(c, 227000011, lat0 + 3 * nm_lat, lon0, 180, 5, 0, now);
    CHECK(n2k_ais_threat(c, now, 926, 1200, 0, NULL, 0, &t) == 2 && t.mmsi == 227000011);
    n2k_set_ais_ignore_still(c, 0);
    CHECK(n2k_ais_threat(c, now, 926, 1200, 0, NULL, 0, &t) == 2 && t.mmsi == 227000010);
    n2k_destroy(c);
}

/* AIS : cible en route de collision (dangereuse), cible parallèle (ignorée),
 * nom en 129809, balise SART active et MOB en test. */
static void test_ais(void)
{
    n2k_ctx *c = n2k_create();
    n2k_ais_target t;
    uint64_t now = 10000;
    const double lat0 = 47.0, lon0 = -3.0, nm_lat = 1.0 / 60.0;
    const double nm_lon = nm_lat / cos(lat0 * 3.14159265358979323846 / 180.0);
    CHECK(n2k_ais_threat(c, now, 926, 1200, 0, NULL, 0, &t) == 0);  /* pas de position du bateau */
    /* Notre bateau : cap au Nord, 5 m/s. */
    uint8_t pos[8], cs[8] = { 0xFF, 0xFC, 0, 0, 0xF4, 0x01, 0xFF, 0xFF };
    put32(pos, (uint32_t)(int32_t)lround(lat0 * 1e7));
    put32(pos + 4, (uint32_t)(int32_t)lround(lon0 * 1e7));
    n2k_feed_frame(c, 0x09F80102u, pos, 8, now);
    n2k_feed_frame(c, 0x09F80202u, cs, 8, now);
    /* 1 NM au Nord, cap au Sud, 5 m/s : CPA 0 dans 185 s. */
    ais_pos(c, 227000001, lat0 + nm_lat, lon0, 180, 5, 0, now);
    /* 2 NM à l'Est, même route et vitesse : mouvement relatif nul. */
    ais_pos(c, 227000002, lat0, lon0 + 2 * nm_lon, 0, 5, -1, now);
    /* Nom de la première (129809 : MMSI @1, nom @5). */
    uint8_t st[27];
    memset(st, 0xFF, sizeof st);
    st[0] = 24;
    put32(st + 1, 227000001);
    memcpy(st + 5, "TEST BOAT@@@@@@@@@@@", 20);
    feed_fast(c, 0x19FB11B2u, st, 27, now);
    CHECK(n2k_ais_count(c, now) == 2);
    CHECK(n2k_ais_threat(c, now, 926, 1200, 0, NULL, 0, &t) == 2);
    CHECK(t.mmsi == 227000001);
    CHECK(strcmp(t.name, "TEST BOAT") == 0);
    CHECK_NEAR(t.range, 1852, 2);
    CHECK_NEAR(t.bearing, 0, 1e-3);
    CHECK_NEAR(t.tcpa, 185.2, 0.5);
    CHECK_NEAR(t.cpa, 0, 2);
    /* Portée de surveillance : cible à 1 NM ignorée sous 1 000 m, gardée à 2 000 m. */
    CHECK(n2k_ais_threat(c, now, 926, 1200, 1000, NULL, 0, &t) == 0);
    CHECK(n2k_ais_threat(c, now, 926, 1200, 2000, NULL, 0, &t) == 2);
    /* Deuxième cible dangereuse plus lointaine (1,5 NM au Nord, cap au Sud) :
     * la première acquittée, on passe à celle-ci. */
    ais_pos(c, 227000003, lat0 + 1.5 * nm_lat, lon0, 180, 5, 0, now);
    uint32_t ack = 227000001;
    CHECK(n2k_ais_threat(c, now, 926, 1200, 0, NULL, 0, &t) == 2 && t.mmsi == 227000001);
    CHECK(n2k_ais_threat(c, now, 926, 1200, 0, &ack, 1, &t) == 2 && t.mmsi == 227000003);
    /* Seuil de CPA dépassé ou TCPA trop lointain. */
    CHECK(n2k_ais_threat(c, now, 926, 100, 0, NULL, 0, &t) == 0);
    /* Extrapolation : 60 s plus tard, la cible s'est rapprochée de 300 m
     * (notre position, elle, n'a pas bougé dans l'état). */
    n2k_feed_frame(c, 0x09F80102u, pos, 8, now + 60000);
    n2k_feed_frame(c, 0x09F80202u, cs, 8, now + 60000);
    CHECK(n2k_ais_threat(c, now + 60000, 926, 1200, 0, NULL, 0, &t) == 2);
    CHECK_NEAR(t.range, 1552, 2);
    /* Détresse : SART active (statut 14) à l'Ouest, MOB en test (statut 15). */
    CHECK(n2k_ais_distress(c, now, NULL, 0, &t) == 0);
    ais_pos(c, 972000001, lat0, lon0 - 0.2 * nm_lon, 0, 0, 15, now);
    CHECK(n2k_ais_distress(c, now, NULL, 0, &t) == 0);
    ais_pos(c, 970123456, lat0, lon0 - 0.5 * nm_lon, 0, 0, 14, now);
    CHECK(n2k_ais_distress(c, now, NULL, 0, &t) == 970);
    CHECK(t.mmsi == 970123456);
    CHECK_NEAR(t.range, 926, 2);
    CHECK_NEAR(t.bearing, 1.5 * 3.14159265358979323846, 1e-3);
    uint32_t sart = 970123456;
    CHECK(n2k_ais_distress(c, now, &sart, 1, &t) == 0);
    /* MOB N2K (127233) : bouton du bord, 0,1 NM au Sud, plus proche que la SART. */
    uint8_t mob[35];
    memset(mob, 0xFF, sizeof mob);
    mob[0] = 0;
    put32(mob + 1, 1);                                  /* identifiant d'émetteur */
    mob[5] = 0xF8 | 1;                                  /* statut 1 : bouton MOB */
    put32(mob + 17, (uint32_t)(int32_t)lround((lat0 - 0.1 * nm_lat) * 1e7));
    put32(mob + 21, (uint32_t)(int32_t)lround(lon0 * 1e7));
    feed_fast(c, 0x09F10102u, mob, 35, now);            /* 127233, src 2 */
    CHECK(n2k_ais_distress(c, now, NULL, 0, &t) == N2K_MOB_N2K);
    CHECK(t.mmsi == 1);
    CHECK_NEAR(t.range, 185.2, 2);
    CHECK_NEAR(t.bearing, 3.14159265358979323846, 1e-3);
    CHECK(n2k_ais_count(c, now) == 5);                  /* le MOB n'est pas une cible AIS */
    /* MOB plus loin que la SART : passe quand même en premier. */
    put32(mob + 17, (uint32_t)(int32_t)lround((lat0 - 2 * nm_lat) * 1e7));
    feed_fast(c, 0x09F10102u, mob, 35, now);
    CHECK(n2k_ais_distress(c, now, NULL, 0, &t) == N2K_MOB_N2K);
    /* MOB mémorisé : statuts inactif et test ignorés, pas de péremption. */
    mob[5] = 0xF8 | 3;
    feed_fast(c, 0x09F10102u, mob, 35, now);
    CHECK(n2k_ais_distress(c, now, NULL, 0, &t) == N2K_MOB_N2K);
    mob[5] = 0xF8 | 2;
    feed_fast(c, 0x09F10102u, mob, 35, now);
    CHECK(n2k_ais_distress(c, now, NULL, 0, &t) == N2K_MOB_N2K);
    CHECK(n2k_ais_distress(c, now + 3600000, NULL, 0, &t) == N2K_MOB_N2K);
    /* 127233 inactif ou en test sans MOB préalable : rien. */
    n2k_ais_reset(c);
    feed_fast(c, 0x09F10102u, mob, 35, now);            /* statut 2 */
    mob[5] = 0xF8 | 3;
    put32(mob + 1, 2);
    feed_fast(c, 0x09F10102u, mob, 35, now);
    CHECK(n2k_ais_distress(c, now, NULL, 0, &t) == 0);
    /* Balise sans point GNSS : alerte immédiate, position inconnue ; une cible
     * ordinaire sans position est ignorée. */
    n2k_ais_reset(c);
    uint8_t np[28];
    memset(np, 0xFF, sizeof np);
    np[0] = 1;
    put32(np + 1, 972600001);
    put32(np + 5, 0x6BE26880u);                         /* 181° : indisponible */
    put32(np + 9, 0x363D7F80u);                         /* 91° : indisponible */
    np[25] = 0xF0 | 14;
    feed_fast(c, 0x11F80EB2u, np, 28, now);
    put32(np + 1, 227000009);
    feed_fast(c, 0x11F80EB2u, np, 28, now);
    CHECK(n2k_ais_count(c, now) == 1);
    CHECK(n2k_ais_distress(c, now, NULL, 0, &t) == 972);
    /* Placée à la position du bateau à la réception, marquée estimée. */
    CHECK(t.mmsi == 972600001 && t.est && t.range < 1);
    {   /* SART sans position : pas d'estimation (souvent reçue de loin). */
        put32(np + 1, 970600003);
        feed_fast(c, 0x11F80EB2u, np, 28, now);
        uint32_t sk = 972600001;
        CHECK(n2k_ais_distress(c, now, &sk, 1, &t) == 970);
        CHECK(t.mmsi == 970600003 && isnan(t.range) && !t.est);
        np[25] = 0xF0 | 15;                             /* puis en test : retirée */
        feed_fast(c, 0x11F80EB2u, np, 28, now);
        np[25] = 0xF0 | 14;
    }
    {   /* Sans position du bateau : position inconnue. */
        n2k_ctx *c2 = n2k_create();
        put32(np + 1, 972600001);
        feed_fast(c2, 0x11F80EB2u, np, 28, now);
        n2k_ais_target t2;
        CHECK(n2k_ais_distress(c2, now, NULL, 0, &t2) == 972);
        CHECK(isnan(t2.range) && isnan(t2.bearing) && !t2.est);
        n2k_destroy(c2);
    }
    /* Puis le point GNSS : position connue, gardée si un message suivant n'en a plus. */
    ais_pos(c, 972600001, lat0 - 0.1 * nm_lat, lon0, 0, 0, 14, now);
    CHECK(n2k_ais_distress(c, now, NULL, 0, &t) == 972);
    CHECK_NEAR(t.range, 185.2, 2);
    CHECK(!t.est);
    put32(np + 1, 972600001);
    feed_fast(c, 0x11F80EB2u, np, 28, now + 1000);
    CHECK(n2k_ais_distress(c, now + 1000, NULL, 0, &t) == 972);
    CHECK_NEAR(t.range, 185.2, 2);
    /* 129802 « MOB TEST » : alerte levée, même mémorisée, et pas de retour après 10 min. */
    uint8_t sf[20];
    memset(sf, 0xFF, sizeof sf);
    sf[0] = 14;
    put32(sf + 1, 972600001);
    sf[5] = 0;
    sf[6] = 2 + 8; sf[7] = 1;
    memcpy(sf + 8, "MOB TEST", 8);
    feed_fast(c, 0x15FB0AB2u, sf, 16, now + 2000);      /* 129802, src 178 */
    CHECK(n2k_ais_distress(c, now + 2000, NULL, 0, &t) == 0);
    ais_pos(c, 972600001, lat0 - 0.1 * nm_lat, lon0, 0, 0, 14, now + 3000);
    CHECK(n2k_ais_distress(c, now + 3000, NULL, 0, &t) == 0);
    CHECK(n2k_ais_distress(c, now + 3000 + 600001, NULL, 0, &t) == 0);
    /* Après le délai, une vraie activation (statut 14) redéclenche. */
    ais_pos(c, 972600001, lat0 - 0.1 * nm_lat, lon0, 0, 0, 14, now + 700000);
    CHECK(n2k_ais_distress(c, now + 700000, NULL, 0, &t) == 972);
    /* « EPIRB TEST » pendant le test, puis « EPIRB ACTIVE » : alerte. */
    ais_pos(c, 974600002, lat0, lon0, 0, 0, 14, now);
    put32(sf + 1, 974600002);
    sf[6] = 2 + 10;
    memcpy(sf + 8, "EPIRB TEST", 10);
    feed_fast(c, 0x15FB0AB2u, sf, 18, now);
    uint32_t mob1 = 972600001;
    CHECK(n2k_ais_distress(c, now + 1000, &mob1, 1, &t) == 0);
    sf[6] = 2 + 12;
    memcpy(sf + 8, "EPIRB ACTIVE", 12);
    feed_fast(c, 0x15FB0AB2u, sf, 20, now + 1000);
    ais_pos(c, 974600002, lat0, lon0, 0, 0, 14, now + 1000);
    CHECK(n2k_ais_distress(c, now + 1000, &mob1, 1, &t) == 974);
    /* Cible oubliée après 6 min sans position. */
    n2k_ais_reset(c);
    ais_pos(c, 970123456, lat0, lon0 - 0.5 * nm_lon, 0, 0, 14, now);
    CHECK(n2k_ais_distress(c, now + 360001, NULL, 0, &t) == 0);
    CHECK(n2k_ais_count(c, now + 360001) == 0);
    n2k_destroy(c);
}

/* Capture réelle à quai : cibles AIS décodées, sans menace (pas de route). */
static void test_ais_capture(const char *path)
{
    FILE *f = fopen(path, "r");
    CHECK(f != NULL);
    if (!f) return;
    n2k_ctx *c = n2k_create();
    char line[256];
    uint64_t t = 0;
    while (fgets(line, sizeof line, f)) {
        n2k_feed_line(c, line, t);
        t += 5;
    }
    fclose(f);
    int n = n2k_ais_count(c, t);
    printf("  AIS capture réelle : %d cibles\n", n);
    CHECK(n > 10);
    n2k_destroy(c);
}

int main(int argc, char **argv)
{
    test_parse();
    test_id();
    test_decoders();
    test_not_available();
    test_fastpacket();
    test_sources();
    test_garbage();
    test_qtvlm_nav();
    test_wp_name();
    test_wp_name_accents();
    test_ais();
    test_ais_still();
    if (argc >= 2) {
        char a[512], b[512];
        snprintf(a, sizeof a, "%s/sim-30s.ydraw", argv[1]);
        snprintf(b, sizeof b, "%s/reel-gps-ais-30s.ydraw", argv[1]);
        test_captures(a, b);
        test_ais_capture(b);
        test_polar(argv[1]);
        test_derived(argv[1]);
        test_current_alea(argv[1]);
        test_hyst_capture(argv[1]);
        test_laylines(argv[1]);
        test_laylines_ground(argv[1]);
        test_leeway(argv[1]);
        test_twa_vmg_pct(argv[1]);
        test_route_wp(argv[1]);
        test_vmg_profile(argv[1]);
    }
    printf("%d vérifications, %d échecs\n", checks, fails);
    return fails ? 1 : 0;
}
