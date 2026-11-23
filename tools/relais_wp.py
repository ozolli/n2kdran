#!/usr/bin/env python3
"""Relais de test : flux YDRAW d'o3nav (lecture seule) + trames 129284 vers un WP
fixe placé à 3 NM au 225 de la première position reçue. Écoute 127.0.0.1:2700.

n2k-sim n'émet pas 129284 : ce relais sert à tester l'affichage WP.
  python3 tools/relais_wp.py &
  adb reverse tcp:2700 tcp:2700
  adb shell am start -S -n oz.n2kdran/.MainActivity --es host 127.0.0.1
"""
import math, socket, threading, time, sys

SRC = ("192.168.0.155", 2700)
pos = {}          # dernière position (lat, lon) en degrés
twd = [None]
wp = {}

def s32(b): v = int.from_bytes(b, "little"); return v - (1 << 32) if v & 0x80000000 else v

def ydraw_lines(id29, payload, seq):
    out, idx, got = [], 0, 0
    tod = time.strftime("%H:%M:%S") + ".000"
    while got < len(payload):
        if idx == 0:
            f = bytes([seq << 5, len(payload)]) + payload[:6]; got = 6
        else:
            f = bytes([(seq << 5) | idx]) + payload[got:got + 7]; got += 7
        f = f + b"\xff" * (8 - len(f))
        out.append(f"{tod} R {id29:08X} " + " ".join(f"{x:02X}" for x in f) + "\r\n")
        idx += 1
    return out

def navdata(dtw_m, btw_rad):
    m = bytearray(b"\xff" * 34)
    m[1:5] = int(round(dtw_m / 0.01)).to_bytes(4, "little")
    m[5] = 0xF0
    m[14:16] = int(round((btw_rad % (2 * math.pi)) / 1e-4)).to_bytes(2, "little")
    return bytes(m)

def main():
    srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 2700)); srv.listen(1)
    print("relais prêt sur 127.0.0.1:2700", flush=True)
    while True:
        cli, _ = srv.accept(); print("client connecté", flush=True)
        up = socket.create_connection(SRC); f = up.makefile("r", encoding="ascii", newline="")
        seq, last = 0, 0.0
        try:
            for line in f:
                parts = line.split()
                if len(parts) >= 3:
                    i = int(parts[2], 16); pgn = ((i >> 8) & 0x1FFFF) if ((i >> 16) & 0xFF) >= 240 else ((i >> 8) & 0x1FF00)
                    d = bytes(int(x, 16) for x in parts[3:])
                    if pgn == 129025 and len(d) == 8:
                        pos["lat"], pos["lon"] = s32(d[0:4]) * 1e-7, s32(d[4:8]) * 1e-7
                cli.sendall(line.encode("ascii"))
                now = time.time()
                if "lat" in pos and now - last >= 1.0:
                    last = now
                    if not wp:     # WP à 3 NM au 225 (vent du simulateur) de la position de départ
                        b, dist = math.radians(225), 3 * 1852
                        wp["lat"] = pos["lat"] + dist * math.cos(b) / 111120
                        wp["lon"] = pos["lon"] + dist * math.sin(b) / (111120 * math.cos(math.radians(pos["lat"])))
                        print(f"WP {wp['lat']:.5f} {wp['lon']:.5f}", flush=True)
                    dn = (wp["lat"] - pos["lat"]) * 111120
                    de = (wp["lon"] - pos["lon"]) * 111120 * math.cos(math.radians(pos["lat"]))
                    for l in ydraw_lines((3 << 26) | (0x1F904 << 8) | 7, navdata(math.hypot(de, dn), math.atan2(de, dn)), seq):
                        cli.sendall(l.encode("ascii"))
                    seq = (seq + 1) & 7
        except (BrokenPipeError, ConnectionResetError):
            print("client parti", flush=True)
        finally:
            up.close(); cli.close()

main()
