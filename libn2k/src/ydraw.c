/*
 * ydraw.c — Analyse d'une ligne YDRAW et décodage de l'identifiant CAN 29 bits.
 *
 * Format (cf. n2k-mux/src/ydraw.c) :
 *   hh:mm:ss.ddd R <ID hex, 29 bits> <octet hex> ... (0 à 8 octets) [CR] [LF]
 * ou format « passerelle » (sortie N2K de qtVlm), sans horodatage ni direction :
 *   <ID hex, 29 bits> <octet hex> ... [CR] [LF]
 * Parseur strict : toute ligne qui s'écarte du format est rejetée sans effet.
 */
#include "n2k_internal.h"

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* Lit exactement n chiffres décimaux. */
static int digits(const char **p, int n, unsigned *out)
{
    unsigned v = 0;
    for (int i = 0; i < n; i++) {
        char c = (*p)[i];
        if (c < '0' || c > '9')
            return 0;
        v = v * 10u + (unsigned)(c - '0');
    }
    *p += n;
    *out = v;
    return 1;
}

static int is_end(char c)
{
    return c == '\0' || c == '\r' || c == '\n';
}

int n2k_parse_ydraw(const char *line, uint32_t *tod_ms, uint32_t *can_id,
                    uint8_t data[8], int *len)
{
    const char *p = line;
    unsigned h, m, s, ms;

    if (!p)
        return 0;
    /* Format « passerelle » (qtVlm) : identifiant puis octets, sans horodatage
     * ni direction. Reconnu à l'absence de « : » en 3e position. */
    if (p[0] && p[1] && p[2] != ':') {
        h = m = s = ms = 0;
        goto ident;
    }
    if (!digits(&p, 2, &h) || *p++ != ':' ||
        !digits(&p, 2, &m) || *p++ != ':' ||
        !digits(&p, 2, &s) || *p++ != '.' ||
        !digits(&p, 3, &ms))
        return 0;
    if (h > 23 || m > 59 || s > 59)
        return 0;

    if (*p != ' ')
        return 0;
    while (*p == ' ')
        p++;
    if (*p != 'R' && *p != 'T')     /* n2k-mux n'émet que R ; T toléré */
        return 0;
    p++;
    if (*p != ' ')
        return 0;
    while (*p == ' ')
        p++;

ident:;
    uint32_t id = 0;
    int nd = 0;
    for (; hexval(*p) >= 0; p++) {
        if (++nd > 8)
            return 0;
        id = (id << 4) | (uint32_t)hexval(*p);
    }
    if (nd == 0 || id > 0x1FFFFFFFu)
        return 0;

    int n = 0;
    while (!is_end(*p)) {
        if (*p != ' ')
            return 0;
        while (*p == ' ')
            p++;
        if (is_end(*p))
            break;                  /* espaces en fin de ligne */
        int hi = hexval(p[0]);
        int lo = (hi >= 0) ? hexval(p[1]) : -1;
        if (lo < 0 || n >= 8)
            return 0;
        if (p[2] != ' ' && !is_end(p[2]))
            return 0;
        data[n++] = (uint8_t)((hi << 4) | lo);
        p += 2;
    }
    while (*p == '\r' || *p == '\n')
        p++;
    if (*p != '\0')
        return 0;

    if (tod_ms)
        *tod_ms = ((h * 60u + m) * 60u + s) * 1000u + ms;
    *can_id = id;
    *len = n;
    return 1;
}

void n2k_decode_id(uint32_t can_id, int *prio, int *pgn, int *src, int *dst)
{
    can_id &= 0x1FFFFFFFu;
    unsigned pf = (can_id >> 16) & 0xFFu;
    unsigned ps = (can_id >> 8) & 0xFFu;
    unsigned dp = (can_id >> 24) & 0x3u;    /* EDP + DP */
    int g, d;

    if (pf < 240) {                         /* PDU1 : adressé, PS = destination */
        g = (int)((dp << 16) | (pf << 8));
        d = (int)ps;
    } else {                                /* PDU2 : diffusion, PS dans le PGN */
        g = (int)((dp << 16) | (pf << 8) | ps);
        d = 255;
    }
    if (prio) *prio = (int)((can_id >> 26) & 0x7u);
    if (pgn)  *pgn  = g;
    if (src)  *src  = (int)(can_id & 0xFFu);
    if (dst)  *dst  = d;
}
