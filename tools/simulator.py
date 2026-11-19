#!/usr/bin/env python3
"""
tools/simulator.py - Simulatore software dei "Giochi di Luce 8x32"

Serve la VERA pagina web (cartella ../data) e parla ESATTAMENTE il
protocollo JSON del firmware v1.5: serve per provare l'interfaccia e il
protocollo senza avere la scheda ESP32-S2 sul tavolo.

Endpoint implementati (identici al firmware):
  GET  /                 -> data/index.html + style.css + app.js
  GET  /effects          -> {"effects":[...], "palettes":[...],
                             "firstAudioFx":17, "fxCount":27, "paletteCount":9}
  GET  /debug            -> diagnostica JSON (heap, parametri, livello, beat...)
  WS   /ws               -> {"type":"status", ...} subito alla connessione,
                            poi {"type":"audio", ...} a 10 Hz e status ogni 5 s
  WS   comandi ricevuti  -> {"cmd":"set", effect?, random?, brightness?,
                             speed?, sensitivity?, gainDb?, palette?}
                            {"cmd":"get"}

Il segnale audio e' SINTETICO (non c'e' microfono): si sceglie con --mode.

Uso:
    pip install -r tools/requirements.txt
    python tools/simulator.py                      # tono 1 kHz, porta 8080
    python tools/simulator.py --mode noise         # rumore "musicale"
    python tools/simulator.py --mode silence       # per vedere la UI ferma
    python tools/simulator.py --mode clipping      # VU meter sempre al massimo
    browser -> http://localhost:8080
"""
from __future__ import annotations

import argparse
import asyncio
import json
import math
import random
import sys
import time
from pathlib import Path

try:
    from aiohttp import web
except ImportError:
    sys.stderr.write(
        "ERRORE: manca il pacchetto aiohttp.\n"
        "  pip install -r tools/requirements.txt\n"
    )
    sys.exit(1)

DATA_DIR = Path(__file__).resolve().parent.parent / "data"

# --- Copiati 1:1 da src/main.cpp (nomi mostrati dalla pagina web) ---
FX_NAMES = [
    "Spento", "Arcobaleno", "Plasma", "Fuoco", "Pioggia Digitale",
    "Onde RGB", "Scintille", "Comete", "Coriandoli", "Interferenza",
    "Fuochi d'Artificio", "Metaball", "Palle Rimbalzanti", "Lampada Lava",
    "Onde Concentriche", "Gradiente Pulsante", "Serpenti",
    "Barre di Spettro", "Barre Centrali", "VU Meter", "Lampo sul Beat",
    "Organo di Colori", "Fuoco Reattivo", "Arcobaleno Reattivo",
    "Anelli Reattivi", "Blocchi EQ", "Palla Audio",
]
PALETTE_NAMES = [
    "Arcobaleno", "Festa", "Oceano", "Lava", "Foresta",
    "Neon", "Tramonto", "Ghiaccio", "Pastello",
]
FIRST_AUDIO_FX = 17          # primo indice audio-reattivo
FX_COUNT = len(FX_NAMES)

# --- Limiti identici a include/config.h (e a PARAM_CONFIG in data/app.js) ---
PARAM_LIMITS = {
    "brightness":  (5, 255, 110),
    "speed":       (1, 100, 50),
    "sensitivity": (0, 100, 50),
    "gainDb":      (0, 36, 12),
    "palette":     (0, len(PALETTE_NAMES) - 1, 0),
}

RANDOM_PERIOD_S = 30.0       # come RANDOM_PERIOD_MS in config.h
TELEMETRY_HZ = 10            # come il loop(): un messaggio "audio" ogni 100 ms
HEARTBEAT_S = 5.0            # come il loop(): uno "status" ogni 5 s


# ==================================================================
# STATO SIMULATO (per il firmware: struct Params + struct AudioState)
# ==================================================================
class SimState:
    def __init__(self, mode: str, freq: float):
        # Parametri modificabili dalla pagina web
        self.effect = 1                      # all'avvio: Arcobaleno
        self.random_on = False
        self.brightness = PARAM_LIMITS["brightness"][2]
        self.speed = PARAM_LIMITS["speed"][2]
        self.sensitivity = PARAM_LIMITS["sensitivity"][2]
        self.gain_db = PARAM_LIMITS["gainDb"][2]
        self.palette = PARAM_LIMITS["palette"][2]

        # Segnale audio sintetico
        self.mode = mode
        self.freq = freq
        self.level = 0.0
        self.bass = 0.0
        self.mid = 0.0
        self.treble = 0.0
        self.beat_env = 0.0
        self.beats = 0
        self.signal = False

        # Contabilita' interna
        self.clients: set = set()
        self._t = 0.0
        self._last_beat = 0.0
        self._last_status = time.time()
        self._last_random = time.time()
        self.uptime0 = time.time()
        self.status_dirty = True

    # ---------------- comandi della pagina web ----------------
    def handle_set(self, obj: dict) -> None:
        changed = False
        for key, (lo, hi, _default) in PARAM_LIMITS.items():
            if key in obj and isinstance(obj[key], (int, float)):
                value = max(lo, min(hi, int(obj[key])))
                if value != getattr(self, key):
                    setattr(self, key, value)
                    changed = True
        if "effect" in obj and isinstance(obj["effect"], (int, float)):
            value = max(0, min(FX_COUNT - 1, int(obj["effect"])))
            if value != self.effect:
                self.effect = value
                changed = True
        if "random" in obj and isinstance(obj["random"], bool):
            if obj["random"] != self.random_on:
                self.random_on = obj["random"]
                self._last_random = time.time()
                changed = True
        if changed:
            self.status_dirty = True

    # ---------------- messaggi verso il browser ----------------
    def status_dict(self) -> dict:
        return {
            "type": "status",
            "effect": self.effect,
            "random": self.random_on,
            "brightness": self.brightness,
            "speed": self.speed,
            "sensitivity": self.sensitivity,
            "gainDb": self.gain_db,
            "palette": self.palette,
            "fxCount": FX_COUNT,
            "heap": 28400,          # valore tipico della heap interna libera
        }

    def audio_dict(self) -> dict:
        return {
            "type": "audio",
            "level": int(round(self.level * 100)),
            "bass": int(round(self.bass * 100)),
            "mid": int(round(self.mid * 100)),
            "treble": int(round(self.treble * 100)),
            "beats": self.beats,
            "signal": self.signal,
        }

    def debug_dict(self) -> dict:
        return {
            "heap": 28400,
            "heap_min": 22150,
            "heap_largest": 18432,
            "psram": 2000000,
            "ws_clients": len(self.clients),
            "effect": self.effect,
            "random": self.random_on,
            "brightness": self.brightness,
            "speed": self.speed,
            "sensitivity": self.sensitivity,
            "gain_db": self.gain_db,
            "palette": self.palette,
            "level": int(round(self.level * 100)),
            "beats": self.beats,
            "raw_peak": 0,           # simulatore: nessun microfono reale
            "raw0": 0,
            "rd_ok": 0,
            "rd_fail": 0,
            "uptime_ms": int((time.time() - self.uptime0) * 1000),
            "rssi": -55,
            "ip": "127.0.0.1 (simulatore)",
        }

    # ---------------- generatore del segnale ----------------
    def tick(self, dt: float) -> None:
        """Aggiorna i valori audio come farebbe audio_task() sul chip."""
        self._t += dt
        t = self._t

        if self.mode == "silence":
            raw_lvl = raw_bass = raw_mid = raw_treb = 0.0
        elif self.mode == "noise":
            raw_lvl = 0.42 + 0.20 * math.sin(t * 3.1) + 0.10 * random.random()
            raw_bass = raw_lvl * (0.55 + 0.35 * math.sin(t * 1.7))
            raw_mid = raw_lvl * (0.45 + 0.30 * math.sin(t * 2.9 + 1.0))
            raw_treb = raw_lvl * (0.35 + 0.35 * math.sin(t * 5.3 + 2.0))
        elif self.mode == "clipping":
            raw_lvl = 0.97
            raw_bass = 0.95
            raw_mid = 0.88
            raw_treb = 0.80
        else:  # "sine": tono puro, l'energia va dove cade la frequenza
            raw_lvl = 0.55 + 0.10 * math.sin(t * 2.0)
            if self.freq < 250.0:
                raw_bass, raw_mid, raw_treb = raw_lvl, raw_lvl * 0.12, raw_lvl * 0.05
            elif self.freq < 2000.0:
                raw_bass, raw_mid, raw_treb = raw_lvl * 0.15, raw_lvl, raw_lvl * 0.10
            else:
                raw_bass, raw_mid, raw_treb = raw_lvl * 0.05, raw_lvl * 0.12, raw_lvl

        # La sensibilita' (0-100) amplifica la reazione, come sul firmware
        gain = 0.55 + (self.sensitivity / 100.0) * 0.9
        self.level = min(1.0, max(0.0, raw_lvl * gain))
        self.bass = min(1.0, max(0.0, raw_bass * gain))
        self.mid = min(1.0, max(0.0, raw_mid * gain))
        self.treble = min(1.0, max(0.0, raw_treb * gain))

        # Rilevazione battito: bassi sopra soglia, non piu' spesso di 140 ms
        # (stessa logica di audio_task: soglia + refrattarieta').
        if self.bass > 0.35 and (t - self._last_beat) > 0.42:
            self._last_beat = t
            self.beat_env = 1.0
            self.beats += 1
        else:
            self.beat_env *= 0.80

        self.signal = self.level > 0.02

        # Cambio effetto casuale ogni 30 s (RANDOM_PERIOD_MS)
        if self.random_on and (time.time() - self._last_random) > RANDOM_PERIOD_S:
            self._last_random = time.time()
            new_effect = self.effect
            for _ in range(6):
                if new_effect != self.effect:
                    break
                new_effect = random.randint(1, FX_COUNT - 1)   # mai "Spento"
            self.effect = new_effect
            self.status_dirty = True


SIM: "SimState"


# ==================================================================
# HANDLER HTTP / WEBSOCKET
# ==================================================================
MIME = {
    ".html": ("text/html", "utf-8"),
    ".css": ("text/css", "utf-8"),
    ".js": ("application/javascript", "utf-8"),
    ".json": ("application/json", None),
    ".png": ("image/png", None),
    ".svg": ("image/svg+xml", "utf-8"),
    ".ico": ("image/x-icon", None),
}
CACHE: dict[str, bytes] = {}


async def static_handler(request: web.Request) -> web.Response:
    """Serve i file della cartella data/ (la stessa caricata in LittleFS)."""
    rel = "index.html" if request.path in ("", "/") else request.path.lstrip("/")
    path = (DATA_DIR / rel).resolve()
    if DATA_DIR not in path.parents and path != DATA_DIR:
        return web.Response(text="403", status=403)          # niente path traversal
    if not path.is_file():
        return web.Response(
            text=(
                "404: file non trovato in data/.\n"
                "Sul firmware reale questo significa che il filesystem LittleFS "
                "non e' stato caricato (pio run -t uploadfs).\n"
            ),
            status=404, content_type="text/plain", charset="utf-8",
        )
    key = str(path)
    if key not in CACHE:
        CACHE[key] = path.read_bytes()                        # file piccoli: in RAM
    ctype, charset = MIME.get(path.suffix.lower(), ("application/octet-stream", None))
    kwargs = {"body": CACHE[key], "content_type": ctype,
              "headers": {"Cache-Control": "no-cache"}}
    if charset:
        kwargs["charset"] = charset
    return web.Response(**kwargs)


async def effects_handler(request: web.Request) -> web.Response:
    return web.json_response({
        "effects": FX_NAMES,
        "palettes": PALETTE_NAMES,
        "firstAudioFx": FIRST_AUDIO_FX,
        "fxCount": FX_COUNT,
        "paletteCount": len(PALETTE_NAMES),
    })


async def debug_handler(request: web.Request) -> web.Response:
    return web.json_response(SIM.debug_dict())


async def ws_handler(request: web.Request) -> web.StreamResponse:
    ws = web.WebSocketResponse(max_msg_size=16 * 1024)
    await ws.prepare(request)
    SIM.clients.add(ws)
    print(f"[WS] client connesso ({len(SIM.clients)} attivi)")
    try:
        await ws.send_str(json.dumps(SIM.status_dict()))       # come il firmware
        async for msg in ws:
            if msg.type != web.WSMsgType.TEXT:
                continue
            try:
                obj = json.loads(msg.data)
            except json.JSONDecodeError:
                continue
            if not isinstance(obj, dict):
                continue
            cmd = obj.get("cmd")
            if cmd == "set":
                SIM.handle_set(obj)
            elif cmd == "get":
                await ws.send_str(json.dumps(SIM.status_dict()))
    finally:
        SIM.clients.discard(ws)
        print(f"[WS] client disconnesso ({len(SIM.clients)} attivi)")
    return ws


# ==================================================================
# LOOP PERIODICO: audio 10 Hz + status 5 s
# ==================================================================
async def telemetry_task() -> None:
    period = 1.0 / TELEMETRY_HZ
    last = time.monotonic()
    while True:
        await asyncio.sleep(period)
        now = time.monotonic()
        SIM.tick(now - last)
        last = now

        dead = [w for w in SIM.clients if w.closed]
        for w in dead:
            SIM.clients.discard(w)
        if not SIM.clients:
            continue

        payload = json.dumps(SIM.audio_dict())
        for w in list(SIM.clients):
            try:
                await w.send_str(payload)
            except Exception:
                SIM.clients.discard(w)

        if SIM.status_dirty or (time.time() - SIM._last_status) > HEARTBEAT_S:
            SIM.status_dirty = False
            SIM._last_status = time.time()
            status = json.dumps(SIM.status_dict())
            for w in list(SIM.clients):
                try:
                    await w.send_str(status)
                except Exception:
                    SIM.clients.discard(w)


async def start_tasks(app: web.Application) -> None:
    app["telemetry"] = asyncio.create_task(telemetry_task())


async def stop_tasks(app: web.Application) -> None:
    app["telemetry"].cancel()
    try:
        await app["telemetry"]
    except asyncio.CancelledError:
        pass


# ==================================================================
# MAIN
# ==================================================================
def main() -> None:
    global SIM
    parser = argparse.ArgumentParser(
        description="Simulatore dei Giochi di Luce 8x32 (protocollo identico al firmware)")
    parser.add_argument("--port", type=int, default=8080, help="porta HTTP/WS (default 8080)")
    parser.add_argument("--host", type=str, default="127.0.0.1",
                        help="interfaccia di ascolto (default 127.0.0.1)")
    parser.add_argument("--mode", type=str, default="sine",
                        choices=["sine", "noise", "silence", "clipping"],
                        help="segnale audio simulato (default sine)")
    parser.add_argument("--freq", type=float, default=1000.0,
                        help="frequenza del tono per --mode sine (default 1000 Hz)")
    args = parser.parse_args()

    SIM = SimState(mode=args.mode, freq=args.freq)

    app = web.Application()
    app.router.add_get("/effects", effects_handler)
    app.router.add_get("/debug", debug_handler)
    app.router.add_get("/ws", ws_handler)
    app.router.add_get("/{tail:.*}", static_handler)
    app.on_startup.append(start_tasks)
    app.on_cleanup.append(stop_tasks)

    print(f"[SIM] File web serviti da: {DATA_DIR}")
    print(f"[SIM] Segnale: {args.mode} ({args.freq:.0f} Hz) | {FX_COUNT} effetti, "
          f"{len(PALETTE_NAMES)} palette")
    print(f"[SIM] Pagina  -> http://localhost:{args.port}")
    print(f"[SIM] Debug   -> http://localhost:{args.port}/debug")
    print(f"[SIM] Effetti -> http://localhost:{args.port}/effects")
    print(f"[SIM] WebSocket -> ws://localhost:{args.port}/ws")
    web.run_app(app, host=args.host, port=args.port, print=None)


if __name__ == "__main__":
    main()
