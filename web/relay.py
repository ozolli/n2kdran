#!/usr/bin/env python3
"""Relais de n2kdran web : sert la page et traduit les flux réseau en WebSocket
(un navigateur ne peut ouvrir ni TCP ni UDP).

  /n2k  flux YDRAW (TCP, lecture seule), ou bus CAN local (SocketCAN, lecture seule,
        trames converties en lignes YDRAW) — une connexion par page ouverte
  /nav  navigation qtVlm (UDP, une ou plusieurs lignes par datagramme), à toutes les pages

Messages envoyés à la page (texte) : lignes YDRAW séparées par LF, ou
« #S hôte:port » (source), « #C » (connecté à la source N2K), « #D <raison> »
(coupé, nouvel essai dans 2 s).

Python 3 seul, sans dépendance.
  python3 web/relay.py --n2k 192.168.0.155:2700 --nav 2800 --http 8090
  (sur o3nav : --n2k 127.0.0.1:2700 ; plusieurs sources : --n2k a:2700,b:2700 ;
   bus CAN de la machine, ex. HALPI : --n2k can:can0)
puis http://localhost:8090/
"""
import argparse
import asyncio
import base64
import hashlib
import os
import socket
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

# Fichiers servis : ceux de web/ (hors sources) et quelques ressources de l'appli,
# prises dans web/ si elles y sont (paquet autonome), sinon à leur place dans le dépôt.
EXTRA = {
    "cm50_outline.png": "app/src/main/res/drawable/cm50_outline.png",
    "icon.png": "docs/icone/n2kdran-rose-1080.png",
    "CM50.pol": "app/src/main/assets/polars/CM50.pol",
    "CM50.saildef": "app/src/main/assets/sailchart/CM50.saildef",
    "CM50.sailselect": "app/src/main/assets/sailchart/CM50.sailselect",
}
TYPES = {".html": "text/html; charset=utf-8", ".js": "text/javascript; charset=utf-8",
         ".wasm": "application/wasm", ".png": "image/png", ".css": "text/css; charset=utf-8",
         ".json": "application/manifest+json",
         ".pol": "text/plain; charset=utf-8", ".saildef": "text/plain; charset=utf-8",
         ".sailselect": "text/plain; charset=utf-8"}
SERVED_EXT = {".html", ".js", ".wasm", ".png", ".css", ".json"}

nav_clients = set()


def ws_frame(text: str) -> bytes:
    data = text.encode("utf-8")
    n = len(data)
    if n < 126:
        head = struct.pack("!BB", 0x81, n)
    elif n < 65536:
        head = struct.pack("!BBH", 0x81, 126, n)
    else:
        head = struct.pack("!BBQ", 0x81, 127, n)
    return head + data


async def ws_reader(reader, writer):
    """Lit les trames du navigateur jusqu'à la fermeture (la page n'envoie rien d'utile)."""
    try:
        while True:
            h = await reader.readexactly(2)
            op, n = h[0] & 0x0F, h[1] & 0x7F
            if n == 126:
                n = struct.unpack("!H", await reader.readexactly(2))[0]
            elif n == 127:
                n = struct.unpack("!Q", await reader.readexactly(8))[0]
            mask = await reader.readexactly(4) if h[1] & 0x80 else b"\0\0\0\0"
            payload = bytes(b ^ mask[i % 4] for i, b in enumerate(await reader.readexactly(n)))
            if op == 0x8:                                   # fermeture
                return
            if op == 0x9:                                   # ping → pong
                writer.write(struct.pack("!BB", 0x8A, len(payload)) + payload)
    except (asyncio.IncompleteReadError, ConnectionError):
        return


class TcpSource:
    """Flux YDRAW en TCP : rend des blocs de lignes complètes."""
    def __init__(self, host, port):
        self.name = f"{host}:{port}"
        self.host, self.port = host, port

    async def open(self):
        self.r, self.w = await asyncio.wait_for(asyncio.open_connection(self.host, self.port), 3)
        self.rest = b""

    async def read(self):
        while True:
            chunk = await self.r.read(65536)
            if not chunk:
                raise ConnectionError("fermé par la source")
            buf = self.rest + chunk
            cut = buf.rfind(b"\n")
            if cut < 0:
                self.rest = buf
                continue
            self.rest = buf[cut + 1:]
            return buf[:cut + 1].decode("ascii", "replace")

    def close(self):
        self.w.close()


CAN_EFF_FLAG, CAN_RTR_FLAG, CAN_ERR_FLAG = 0x80000000, 0x40000000, 0x20000000
CAN_FRAME = struct.Struct("<IB3x8s")          # struct can_frame (Linux)


def ydraw_line(can_id, data, t):
    """Trame CAN → ligne YDRAW « hh:mm:ss.mmm R ID octets » (heure locale)."""
    lt = time.localtime(t)
    return "%02d:%02d:%02d.%03d R %08X %s\r\n" % (
        lt.tm_hour, lt.tm_min, lt.tm_sec, int(t * 1000) % 1000, can_id,
        " ".join("%02X" % b for b in data))


class CanSource:
    """Bus CAN local (SocketCAN), en lecture seule : rien n'est jamais émis sur le bus.
    Les trames à identifiant 29 bits sont converties en lignes YDRAW, par paquets de 50 ms."""
    def __init__(self, ifname):
        self.name = "can:" + ifname
        self.ifname = ifname

    async def open(self):
        if not hasattr(socket, "AF_CAN"):
            raise OSError("SocketCAN indisponible (Linux seulement)")
        s = socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
        try:
            s.bind((self.ifname,))
            s.setblocking(False)
        except OSError:
            s.close()
            raise
        self.s = s

    async def read(self):
        loop = asyncio.get_running_loop()
        lines = []
        end = None
        while True:
            timeout = None if end is None else max(0.0, end - loop.time())
            try:
                pkt = await asyncio.wait_for(loop.sock_recv(self.s, CAN_FRAME.size), timeout)
            except asyncio.TimeoutError:
                return "".join(lines)
            if len(pkt) < CAN_FRAME.size:
                continue
            cid, n, data = CAN_FRAME.unpack(pkt)
            if not cid & CAN_EFF_FLAG or cid & (CAN_RTR_FLAG | CAN_ERR_FLAG):
                continue                              # N2K : identifiants 29 bits seulement
            lines.append(ydraw_line(cid & 0x1FFFFFFF, data[:min(n, 8)], time.time()))
            if end is None:
                end = loop.time() + 0.05

    def close(self):
        self.s.close()


def make_source(item):
    item = item.strip()
    if item.startswith("can:"):
        return CanSource(item[4:])
    h, _, p = item.rpartition(":")
    return TcpSource(h, int(p))


async def n2k_connect(sources):
    """Première source de la liste qui répond (3 s chacune), ou (None, erreur)."""
    err = None
    for src in sources:
        try:
            await src.open()
            return src, None
        except (OSError, asyncio.TimeoutError) as e:
            err = f"{src.name} : " + (str(e) or type(e).__name__)
    return None, err


async def n2k_session(writer, sources):
    """Relaie la source N2K vers une page ; si elle tombe, la liste est reprise du début."""
    while True:
        src, err = await n2k_connect(sources)
        if not src:
            writer.write(ws_frame("#D " + err))
            await writer.drain()
            await asyncio.sleep(2)
            continue
        writer.write(ws_frame(f"#S {src.name}"))
        writer.write(ws_frame("#C"))
        try:
            while True:
                text = await asyncio.wait_for(src.read(), 5)        # muet 5 s = coupé
                writer.write(ws_frame(text))
                await writer.drain()
        except (OSError, ConnectionError, asyncio.TimeoutError) as e:
            if writer.is_closing():
                src.close()
                return
            writer.write(ws_frame("#D " + (str(e) or type(e).__name__)))
            await writer.drain()
        finally:
            src.close()
        await asyncio.sleep(2)


class NavProtocol(asyncio.DatagramProtocol):
    def datagram_received(self, data, addr):
        msg = ws_frame(data.decode("ascii", "replace"))
        for w in list(nav_clients):
            if w.is_closing():
                nav_clients.discard(w)
            else:
                w.write(msg)


async def handle(reader, writer, args):
    try:
        req = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), 10)
    except (asyncio.IncompleteReadError, asyncio.LimitOverrunError, asyncio.TimeoutError, ConnectionError):
        writer.close()
        return
    lines = req.decode("latin-1").split("\r\n")
    parts = lines[0].split(" ")
    path = parts[1].split("?")[0] if len(parts) > 1 else "/"
    hdr = {}
    for l in lines[1:]:
        if ":" in l:
            k, v = l.split(":", 1)
            hdr[k.strip().lower()] = v.strip()

    if path in ("/n2k", "/nav") and hdr.get("upgrade", "").lower() == "websocket":
        key = hdr.get("sec-websocket-key", "")
        acc = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
        writer.write(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                      "Connection: Upgrade\r\nSec-WebSocket-Accept: " + acc + "\r\n\r\n").encode())
        await writer.drain()
        if path == "/nav":
            nav_clients.add(writer)
            await ws_reader(reader, writer)
            nav_clients.discard(writer)
        else:
            task = asyncio.create_task(n2k_session(writer, [make_source(i) for i in args.items]))
            await ws_reader(reader, writer)
            task.cancel()
        writer.close()
        return

    name = "index.html" if path == "/" else path.lstrip("/")
    if name in EXTRA:
        file = os.path.join(HERE, name)
        if not os.path.isfile(file):
            file = os.path.join(ROOT, EXTRA[name])
    elif "/" not in name and os.path.splitext(name)[1] in SERVED_EXT:
        file = os.path.join(HERE, name)
    else:
        file = None
    if file and os.path.isfile(file):
        with open(file, "rb") as f:
            body = f.read()
        ctype = TYPES.get(os.path.splitext(name)[1], "application/octet-stream")
        writer.write(f"HTTP/1.1 200 OK\r\nContent-Type: {ctype}\r\nContent-Length: {len(body)}\r\n"
                     "Cache-Control: no-cache\r\nConnection: close\r\n\r\n".encode() + body)
    else:
        writer.write(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
    await writer.drain()
    writer.close()


async def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--n2k", default="10.3.0.2:2700",
                    help="source(s) hôte:port (YDRAW en TCP) ou can:<interface> (bus CAN local), "
                         "liste séparée par des virgules essayée dans l'ordre "
                         "(défaut 10.3.0.2:2700 ; sur o3nav : 127.0.0.1:2700 ; HALPI : can:can0)")
    ap.add_argument("--nav", type=int, default=2800, help="port UDP de la navigation qtVlm (0 = aucun)")
    ap.add_argument("--http", type=int, default=8090, help="port de la page (défaut 8090 ; 8080 = interface web de n2k-mux)")
    ap.add_argument("--bind", default="0.0.0.0", help="adresse d'écoute de la page (défaut : toutes)")
    args = ap.parse_args()
    # Une liste d'objets source par page : chacune a sa propre connexion.
    items = [i for i in args.n2k.split(",") if i.strip()]
    for i in items:
        make_source(i)                                  # erreur de syntaxe : dès le départ
    args.items = items

    loop = asyncio.get_running_loop()
    if args.nav:
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
            s.bind(("0.0.0.0", args.nav))
            await loop.create_datagram_endpoint(NavProtocol, sock=s)
        except OSError as e:
            print(f"UDP {args.nav} : {e} (navigation qtVlm indisponible)", file=sys.stderr)
    srv = await asyncio.start_server(lambda r, w: handle(r, w, args), args.bind, args.http)
    print(f"n2kdran web : http://localhost:{args.http}/  (N2K {args.n2k}, UDP {args.nav})", flush=True)
    async with srv:
        await srv.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
