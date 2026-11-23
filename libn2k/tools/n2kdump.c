/*
 * n2kdump — lit du YDRAW sur stdin et affiche l'état décodé par libn2k.
 *
 *   nc 192.168.0.155 2700 | ./n2kdump            (temps réel, horloge monotone)
 *   ./n2kdump -r < capture.ydraw                 (rejeu, temps des horodatages)
 *
 * Options : -r rejeu ; -i MS intervalle d'affichage (0 = seulement à la fin,
 * défaut 1000) ; -s MS délai de péremption (défaut 3000) ; -p FICHIER polaire
 * qtVlm (active vitesse polaire, cibles, plage VMG) ; -w MOY,CIBLE,COURANT
 * fenêtres de moyenne en secondes (défaut 600,60,60). src=-1 : donnée calculée.
 * Code retour : 0, ou 2 si au moins une ligne a été rejetée.
 */
#define _POSIX_C_SOURCE 200809L
#include "n2k.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define RAD2DEG (180.0 / 3.14159265358979323846)
#define MS2KN   (3600.0 / 1852.0)

static uint64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

typedef struct { double k; double off; const char *unit; int dec; } disp;

static disp display_of(n2k_item it)
{
    switch (it) {
    case N2K_LAT: case N2K_LON:          return (disp){ 1, 0, "°", 6 };
    case N2K_COG: case N2K_HDG_TRUE: case N2K_HDG_MAG: case N2K_VARIATION:
    case N2K_PITCH: case N2K_ROLL: case N2K_RUDDER: case N2K_AWA: case N2K_TWA:
    case N2K_GWA: case N2K_GWD: case N2K_SET: case N2K_BTW:
    case N2K_TWD: case N2K_TWD_AVG: case N2K_TARGET_TWA:
    case N2K_VMG_RANGE_MIN: case N2K_VMG_RANGE_MAX:
    case N2K_TWD_MIN: case N2K_TWD_MAX: case N2K_LAY_STBD: case N2K_LAY_PORT:
    case N2K_LAY_STBD_MIN: case N2K_LAY_STBD_MAX:
    case N2K_LAY_PORT_MIN: case N2K_LAY_PORT_MAX:
    case N2K_LAY_STBD_GND: case N2K_LAY_PORT_GND:
    case N2K_LAY_STBD_GND_MIN: case N2K_LAY_STBD_GND_MAX:
    case N2K_LAY_PORT_GND_MIN: case N2K_LAY_PORT_GND_MAX:
    case N2K_LEEWAY: case N2K_LEEWAY_SIGMA:
    case N2K_LAY_STBD_GND_LO: case N2K_LAY_STBD_GND_HI:
    case N2K_LAY_PORT_GND_LO: case N2K_LAY_PORT_GND_HI:
                                         return (disp){ RAD2DEG, 0, "°", 1 };
    case N2K_ROT:                        return (disp){ RAD2DEG * 60, 0, "°/min", 1 };
    case N2K_SOG: case N2K_BSP: case N2K_AWS: case N2K_TWS: case N2K_GWS:
    case N2K_DRIFT: case N2K_WCV: case N2K_TWS_AVG: case N2K_VMG:
    case N2K_POLAR_BSP: case N2K_TARGET_BSP:        return (disp){ MS2KN, 0, "kn", 2 };
    case N2K_LOG: case N2K_TRIP: case N2K_DTW: case N2K_DTW_SAIL:
                                         return (disp){ 1.0 / 1852.0, 0, "NM", 2 };
    case N2K_SEA_TEMP: case N2K_AIR_TEMP:return (disp){ 1, -273.15, "°C", 2 };
    case N2K_PRESSURE:                   return (disp){ 0.01, 0, "hPa", 1 };
    case N2K_HUMIDITY: case N2K_POLAR_PCT: case N2K_VMG_PCT: case N2K_TWA_VMG_PCT:
                                         return (disp){ 1, 0, "%", 1 };
    case N2K_TTW_STBD: case N2K_TTW_PORT: case N2K_TTW:
                                         return (disp){ 1.0 / 60.0, 0, "min", 1 };
    case N2K_DEPTH: case N2K_DEPTH_OFFSET: case N2K_XTE:
                                         return (disp){ 1, 0, "m", 2 };
    default:                             return (disp){ 1, 0, "", 2 };
    }
}

static void print_state(const n2k_ctx *ctx, uint64_t now)
{
    static const char *const st_name[] = { "jamais", "PÉRIMÉ", "ok" };
    for (int i = 0; i < N2K_ITEM_COUNT; i++) {
        double v;
        int src, pgn;
        uint64_t age;
        n2k_status st = n2k_get_info(ctx, (n2k_item)i, now, &src, &pgn, &age);
        if (st == N2K_NEVER)
            continue;
        n2k_get(ctx, (n2k_item)i, now, &v);
        disp d = display_of((n2k_item)i);
        printf("%-13s %12.*f %-6s %-6s src=%-4d pgn=%-6d âge=%llums\n",
               n2k_item_name((n2k_item)i), d.dec, v * d.k + d.off, d.unit,
               st_name[st], src, pgn, (unsigned long long)age);
    }
    printf("--\n");
    fflush(stdout);
}

int main(int argc, char **argv)
{
    int replay = 0, opt;
    const char *polar_path = NULL;
    unsigned w_avg = 0, w_tgt = 0, w_cur = 0;
    long interval = 1000, stale = 3000;

    while ((opt = getopt(argc, argv, "ri:s:p:w:")) != -1) {
        switch (opt) {
        case 'r': replay = 1; break;
        case 'i': interval = strtol(optarg, NULL, 10); break;
        case 's': stale = strtol(optarg, NULL, 10); break;
        case 'p': polar_path = optarg; break;
        case 'w':
            if (sscanf(optarg, "%u,%u,%u", &w_avg, &w_tgt, &w_cur) != 3) {
                fprintf(stderr, "-w attend MOY,CIBLE,COURANT (secondes)\n");
                return 1;
            }
            break;
        default:
            fprintf(stderr, "usage : %s [-r] [-i MS] [-s MS] [-p polaire.pol] [-w s,s,s] < ydraw\n", argv[0]);
            return 1;
        }
    }

    n2k_ctx *ctx = n2k_create();
    if (!ctx)
        return 1;
    n2k_set_stale_ms(ctx, stale > 0 ? (uint32_t)stale : 3000u);
    n2k_set_windows(ctx, w_avg, w_tgt, w_cur);

    n2k_polar *polar = NULL;
    if (polar_path) {
        static char buf[1 << 16];
        char err[128];
        FILE *f = fopen(polar_path, "rb");
        size_t n = f ? fread(buf, 1, sizeof buf, f) : 0;
        if (f)
            fclose(f);
        polar = f ? n2k_polar_parse(buf, n, err, sizeof err) : NULL;
        if (!polar) {
            fprintf(stderr, "polaire %s : %s\n", polar_path, f ? err : "illisible");
            return 1;
        }
        n2k_set_polar(ctx, polar);
    }

    char line[512];
    uint64_t now = 0, next_print = 0, day = 0;
    uint32_t last_tod = 0;
    int have_tod = 0;

    while (fgets(line, sizeof line, stdin)) {
        if (replay) {
            uint32_t tod, id;
            uint8_t d[8];
            int len;
            if (n2k_parse_ydraw(line, &tod, &id, d, &len)) {
                if (have_tod && tod + 12u * 3600u * 1000u < last_tod)
                    day++;                          /* passage de minuit */
                last_tod = tod;
                have_tod = 1;
                now = day * 86400000u + tod;
            }
        } else {
            now = mono_ms();
        }
        n2k_feed_line(ctx, line, now);

        if (interval > 0 && now >= next_print) {
            if (next_print != 0)
                print_state(ctx, now);
            next_print = now + (uint64_t)interval;
        }
    }

    print_state(ctx, now);
    n2k_stats s;
    n2k_get_stats(ctx, &s);
    fprintf(stderr,
            "lignes=%llu rejetées=%llu trames_utiles=%llu ignorées=%llu "
            "fp_ok=%llu fp_abandonnés=%llu erreurs_décodage=%llu\n",
            (unsigned long long)s.lines, (unsigned long long)s.bad_lines,
            (unsigned long long)s.frames_used, (unsigned long long)s.frames_ignored,
            (unsigned long long)s.fp_complete, (unsigned long long)s.fp_dropped,
            (unsigned long long)s.decode_errors);
    n2k_destroy(ctx);
    n2k_polar_free(polar);
    return s.bad_lines ? 2 : 0;
}
