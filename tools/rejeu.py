#!/usr/bin/env python3
"""Rejeu d'une capture YDRAW en TCP, au rythme des horodatages, en boucle.

Remplace o3nav quand il n'est pas joignable (travail hors du bord) :
  python3 tools/rejeu.py tests/data/sim-hyst-900s.ydraw [port]
puis, dans n2kdran, adresse = IP du Gram (ou 127.0.0.1 avec adb reverse).

Les lignes sont réémises avec l'heure courante (format inchangé) ; plusieurs
clients peuvent se connecter.
"""
import socket
import sys
import threading
import time

path = sys.argv[1]
port = int(sys.argv[2]) if len(sys.argv) > 2 else 2700

with open(path, encoding="ascii", errors="replace") as f:
    lines = [l.rstrip("\r\n") for l in f if l.strip()]


def tod_ms(line):
    try:
        h, m, s = line[:12].split(":")
        return (int(h) * 3600 + int(m) * 60) * 1000 + int(float(s) * 1000)
    except ValueError:
        return None


clients = []
lock = threading.Lock()


def accept_loop(srv):
    while True:
        c, a = srv.accept()
        c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        print("client", a, flush=True)
        with lock:
            clients.append(c)


srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("0.0.0.0", port))
srv.listen(8)
threading.Thread(target=accept_loop, args=(srv,), daemon=True).start()
print(f"rejeu de {path} ({len(lines)} lignes) sur TCP {port}", flush=True)

while True:
    t0_file, t0_wall = None, time.monotonic()
    for line in lines:
        t = tod_ms(line)
        if t is not None:
            if t0_file is None:
                t0_file = t
            wait = (t - t0_file) / 1000 - (time.monotonic() - t0_wall)
            if wait > 0:
                time.sleep(wait)
        now = time.strftime("%H:%M:%S") + ".%03d" % (int(time.time() * 1000) % 1000)
        out = (now + line[12:] if t is not None else line) + "\r\n"
        with lock:
            for c in clients[:]:
                try:
                    c.sendall(out.encode("ascii"))
                except OSError:
                    clients.remove(c)
                    c.close()
    print("boucle", flush=True)
