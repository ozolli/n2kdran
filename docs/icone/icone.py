#!/usr/bin/env python3
"""Génère docs/icone/icone.svg (repère 1080) avec le design actuel de la rose :
cercle des allures à l'extérieur (515–540), carte des angles (381–512),
silhouette du CM50, laylines en trait plein avec leurs secteurs d'historique.
Usage : python3 docs/icone/icone.py  (appelé par tools/regen.sh)"""
import math, os

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
RES = os.path.join(ROOT, "app/src/main/res/drawable")
CARD_S = 512 / 437
R_IN = 381 * CARD_S                       # intérieur de la rose
K = 800 / 1080                            # repère virtuel de l'app → repère 1080 : /K
TWA, AWA, WP = -42, -28, 18               # angles depuis l'étrave (°)
GREEN, RED = "#2ECC40", "#FF4136"        # tribord / bâbord (palette jour)


def pt(a, r):                             # a en degrés depuis le haut, sens horaire
    t = math.radians(a)
    return 540 + r * math.sin(t), 540 - r * math.cos(t)


def wedge(a0, a1, color, alpha):
    x0, y0 = pt(a0, R_IN); x1, y1 = pt(a1, R_IN)
    return (f'<path d="M540,540 L{x0:.1f},{y0:.1f} A{R_IN:.1f},{R_IN:.1f} 0 0 1 {x1:.1f},{y1:.1f} Z" '
            f'fill="{color}" fill-opacity="{alpha}"/>')


def allures_color(a):
    """Couleur type du cercle des allures (rouge pincé, vert VMG, bleu travers)."""
    t = abs((a + 540) % 360 - 180)        # TWA, symétrique
    keys = [(0, (255, 0, 0)), (28, (255, 0, 0)), (40, (0, 255, 0)), (62, (0, 255, 0)),
            (78, (0, 0, 255)), (108, (0, 0, 255)), (124, (0, 255, 0)), (158, (0, 255, 0)),
            (172, (255, 0, 0)), (180, (255, 0, 0))]
    for (t0, c0), (t1, c1) in zip(keys, keys[1:]):
        if t <= t1:
            f = (t - t0) / (t1 - t0)
            return "#%02X%02X%02X" % tuple(round(u + f * (v - u)) for u, v in zip(c0, c1))


FONT = 'font-family="Roboto, sans-serif" font-weight="bold" text-anchor="middle"'
R_MID = 409 * CARD_S                      # cercle médian de la carte (pointes des triangles)
R_OUT = 512                               # bord extérieur de la carte


def card():
    """Carte des angles (cf. drawCard) : anneau blanc, graduations 5°, N/E/S/W et dizaines."""
    rm, w = (R_IN + R_OUT) / 2, R_OUT - R_IN
    out = [f'<circle cx="540" cy="540" r="{rm:.1f}" fill="none" stroke="#F4F4F4" stroke-width="{w:.1f}"/>']
    for r in (R_IN, R_OUT):
        out.append(f'<circle cx="540" cy="540" r="{r:.1f}" fill="none" stroke="#606060" stroke-width="{1.5 / K:.1f}"/>')
    for d in range(0, 360, 5):
        if d % 30 == 0:
            continue
        lg = d % 10 == 0
        r1 = R_OUT - 4 / K; r0 = r1 - (16 if lg else 9) / K
        (xa, ya), (xb, yb) = pt(d, r0), pt(d, r1)
        out.append(f'<line x1="{xa:.1f}" y1="{ya:.1f}" x2="{xb:.1f}" y2="{yb:.1f}" '
                   f'stroke="{"#404040" if lg else "#909090"}" stroke-width="{(3 if lg else 2) / K:.1f}" '
                   'stroke-linecap="round"/>')
    for d in range(0, 360, 30):
        txt = {0: "N", 90: "E", 180: "S", 270: "W"}.get(d, str(d))
        size = (34 if d % 90 == 0 else 28) / K
        out.append(f'<text transform="rotate({d} 540 540)" x="540" y="{540 - R_MID + size * 0.36:.1f}" '
                   f'{FONT} font-size="{size:.1f}" fill="#000">{txt}</text>')
    return out


def needle(a, color, letter):
    """Triangle TWA/AWA (cf. drawNeedle) : pointe sur le cercle médian, lettre blanche cernée."""
    tip = 540 - R_MID; base = tip + 99.7; half = 46
    ty = base - 38 + 9 / K
    t = f'x="540" y="{ty:.1f}" {FONT} font-size="{26 / K:.1f}"'
    return [f'<g transform="rotate({a} 540 540)">',
            f'<path d="M540,{tip:.1f} L{540 - half},{base:.1f} L{540 + half},{base:.1f} Z" fill="{color}" '
            f'stroke="#000" stroke-width="{1.5 / K:.1f}" stroke-linejoin="round"/>',
            f'<text {t} stroke="#000" stroke-width="{3 / K:.1f}">{letter}</text>',
            f'<text {t} fill="#FFF">{letter}</text>', '</g>']


def wp_mark(a):
    """Triangle jaune du WP, pointe sur les points gris (cf. drawWpMark)."""
    r, h, half = 409 * CARD_S, 47 / K, 20 / K
    y0, y1 = 540 - r, 540 - r + h
    return (f'<path transform="rotate({a} 540 540)" d="M540,{y0:.1f} L{540 - half:.1f},{y1:.1f} '
            f'L{540 + half:.1f},{y1:.1f} Z" fill="#FFD228" stroke="#000" stroke-width="{1.2 / K:.2f}"/>')


def rudder(deg, lo, hi):
    """Jauge de barre en bas du bateau (cf. drawRudder), barre `deg`, historique lo..hi."""
    r, arc, rng = 205 / K, 28.0, 15.0
    kk = arc / rng
    def p(b, rr):                          # b : angle de barre, bas de la rose, tribord à droite
        return pt(180 - b * kk, rr)
    out = []
    (x0, y0), (x1, y1) = p(-rng, r), p(rng, r)
    out.append(f'<path d="M{x0:.1f},{y0:.1f} A{r:.1f},{r:.1f} 0 0 0 {x1:.1f},{y1:.1f}" '
               f'stroke="#B0B0B0" stroke-width="{3 / K:.1f}" fill="none"/>')
    for d in range(-15, 16, 5):
        ln = (18 if d % 15 == 0 else 11) / K
        (xa, ya), (xb, yb) = p(d, r), p(d, r - ln)
        out.append(f'<line x1="{xa:.1f}" y1="{ya:.1f}" x2="{xb:.1f}" y2="{yb:.1f}" stroke="#B0B0B0" '
                   f'stroke-width="{(4 if d == 0 else 2) / K:.1f}"/>')
    rh = r + 9 / K
    (xa, ya), (xb, yb) = p(lo, rh), p(hi, rh)
    out.append(f'<path d="M{xa:.1f},{ya:.1f} A{rh:.1f},{rh:.1f} 0 0 0 {xb:.1f},{yb:.1f}" '
               f'stroke="{GREEN}" stroke-width="{8 / K:.1f}" fill="none"/>')
    tri = [p(deg, r - 2 / K), pt(180 - deg * kk - 4, r - 30 / K), pt(180 - deg * kk + 4, r - 30 / K)]
    out.append('<path d="M' + " L".join(f"{x:.1f},{y:.1f}" for x, y in tri) + f' Z" fill="{GREEN}"/>')
    out.append(f'<text x="540" y="{540 + (205 - 42) / K:.1f}" font-family="Roboto, sans-serif" font-weight="bold" '
               f'font-size="{30 / K:.1f}" fill="#FFF" text-anchor="middle">{abs(deg)}</text>')
    return out


def main():
    s = ['<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" '
         'width="1080" height="1080" viewBox="0 0 1080 1080">',
         '<rect width="1080" height="1080" fill="#000"/>']
    # Cercle des allures : 360 arcs de 1° (légèrement recouvrants).
    r = 527.5
    for i in range(360):
        x0, y0 = pt(i, r); x1, y1 = pt(i + 1.3, r)
        s.append(f'<path d="M{x0:.2f},{y0:.2f} A{r},{r} 0 0 1 {x1:.2f},{y1:.2f}" '
                 f'stroke="{allures_color(i + 0.5)}" stroke-width="25" fill="none"/>')
    # Silhouette (sous la carte, comme dans l'app), puis carte des angles.
    k = CARD_S
    bx, by = 540 + (380 - 540) * k, 540 + (225 - 540) * k
    s.append(f'<image xlink:href="file://{os.path.join(RES, "cm50_outline.png")}" '
             f'x="{bx:.1f}" y="{by:.1f}" width="{319 * k:.1f}" height="{625 * k:.1f}"/>')
    s += card()
    # Laylines : tribord (vert) avec son secteur, bâbord (rouge) avec historique flou.
    s.append(wedge(-33, -19, GREEN, 0.35))
    for d in range(-8, 9, 2):
        s.append(wedge(62 + d - 7, 62 + d + 7, RED, 0.10))
    for a, col in ((-26, GREEN), (62, RED)):
        x, y = pt(a, R_IN)
        s.append(f'<line x1="540" y1="540" x2="{x:.1f}" y2="{y:.1f}" stroke="{col}" '
                 f'stroke-width="14" stroke-linecap="round"/>')
    s += rudder(3, -2, 5)
    s += needle(AWA, "#4A78B0", "A")
    s += needle(TWA, "#00FF00", "T")
    s.append(wp_mark(WP))
    s.append('</svg>')
    open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "icone.svg"), "w").write("\n".join(s))


main()
