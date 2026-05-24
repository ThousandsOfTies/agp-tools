"""
bridge.py — Hardware simulator web bridge

Unix socket (/tmp/hw_sim.sock) ↔ WebSocket ↔ Browser HTML panel

Devices:
  GPIO : LED (output), Button (input)
  I2C  : VL53L0X distance sensor
  SPI  : MFRC-522 RFID reader, LCD HAT 240x240
"""

import asyncio
import json
import os
import socket
import threading
from pathlib import Path

import websockets
from aiohttp import web

UNIX_SOCK  = "/tmp/hw_sim.sock"
HTTP_PORT  = 8080
WS_PORT    = 8765
PANEL_DIR  = Path(__file__).parent / "panel"

# ------------------------------------------------------------------ #
# Shared state                                                         #
# ------------------------------------------------------------------ #

state = {
    "gpio": {
        "leds":    {18: False, 24: False},
        "buttons": {17: False, 27: False},
    },
    "i2c": {
        "vl53l0x": {"range_mm": 300, "status": 0x01},
        "ssd1306": {"framebuf": None},   # 128x64 monochrome (1024 bytes)
    },
    "spi": {
        "mfrc522": {"uid": None, "present": False},
        "lcd":     {"pixels": None},
    },
}

ws_clients: set = set()

# ------------------------------------------------------------------ #
# WebSocket broadcast                                                  #
# ------------------------------------------------------------------ #

async def broadcast(msg: dict):
    if ws_clients:
        data = json.dumps(msg)
        await asyncio.gather(*[c.send(data) for c in ws_clients],
                             return_exceptions=True)


# ------------------------------------------------------------------ #
# Virtual hardware actions                                             #
# ------------------------------------------------------------------ #

async def set_button(line: int, value: bool):
    state["gpio"]["buttons"][line] = value
    await broadcast({"type": "button", "line": line, "value": value})


async def press_button(line: int, duration_ms: int):
    await set_button(line, True)
    if duration_ms:
        await asyncio.sleep(duration_ms / 1000)
        await set_button(line, False)


async def tap_rfid(uid: str):
    state["spi"]["mfrc522"] = {"uid": uid, "present": True}
    await broadcast({"type": "rfid", "uid": uid, "present": True})


async def remove_rfid():
    state["spi"]["mfrc522"] = {"uid": None, "present": False}
    await broadcast({"type": "rfid", "uid": None, "present": False})


async def set_range(mm: int):
    state["i2c"]["vl53l0x"]["range_mm"] = mm
    await broadcast({"type": "range", "value": mm})

# ------------------------------------------------------------------ #
# WebSocket handler (browser ↔ bridge)                                 #
# ------------------------------------------------------------------ #

async def ws_handler(websocket):
    ws_clients.add(websocket)
    try:
        await websocket.send(json.dumps({"type": "init", "state": state}))
        async for raw in websocket:
            msg = json.loads(raw)
            mtype = msg.get("type")

            if mtype == "button":
                line = msg["line"]
                val  = bool(msg["value"])
                await set_button(line, val)

            elif mtype == "rfid_tap":
                uid = msg.get("uid", "04:AB:CD:EF:01:23")
                await tap_rfid(uid)

            elif mtype == "rfid_remove":
                await remove_rfid()

            elif mtype == "range_set":
                mm = int(msg.get("value", 300))
                await set_range(mm)

    except websockets.ConnectionClosed:
        pass
    finally:
        ws_clients.discard(websocket)

# ------------------------------------------------------------------ #
# Unix socket server (C shim/stub ↔ bridge)                            #
# ------------------------------------------------------------------ #

def handle_stub_message(raw: str, loop) -> str | None:
    """Process a JSON line from a C stub. Returns response string if needed."""
    try:
        msg = json.loads(raw.strip())
    except json.JSONDecodeError:
        return None

    event  = msg.get("event")
    device = msg.get("device")

    if event == "set" and device == "gpio":
        line = msg["line"]
        val  = bool(msg["value"])
        state["gpio"]["leds"][line] = val
        asyncio.run_coroutine_threadsafe(
            broadcast({"type": "led", "line": line, "value": val}), loop)

    elif event == "set" and device == "i2c_range":
        mm = int(msg.get("value", 300))
        state["i2c"]["vl53l0x"]["range_mm"] = mm
        asyncio.run_coroutine_threadsafe(
            broadcast({"type": "range", "value": mm}), loop)

    elif event == "set" and device == "lcd":
        state["spi"]["lcd"]["pixels"] = msg.get("pixels")
        asyncio.run_coroutine_threadsafe(
            broadcast({"type": "lcd", "pixels": msg.get("pixels")}), loop)

    elif event == "set" and device == "oled":
        state["i2c"]["ssd1306"]["framebuf"] = msg.get("framebuf")
        asyncio.run_coroutine_threadsafe(
            broadcast({"type": "oled", "framebuf": msg.get("framebuf")}), loop)

    elif msg.get("req") == "get" and device == "gpio":
        line = msg.get("line")
        val  = int(state["gpio"]["buttons"].get(line, False))
        return json.dumps({"value": val}) + "\n"

    elif msg.get("req") == "get" and device == "rfid":
        rfid = state["spi"]["mfrc522"]
        return json.dumps({
            "present": bool(rfid.get("present")),
            "uid":     rfid.get("uid") or "00:00:00:00",
        }) + "\n"

    elif event == "register":
        print(f"[bridge] register {device} line={msg.get('line')} dir={msg.get('dir')}")

    return None


def unix_server_thread(loop):
    if os.path.exists(UNIX_SOCK):
        os.remove(UNIX_SOCK)

    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(UNIX_SOCK)
    srv.listen(8)
    os.chmod(UNIX_SOCK, 0o666)
    print(f"[bridge] Unix socket listening: {UNIX_SOCK}")

    while True:
        conn, _ = srv.accept()
        threading.Thread(target=handle_stub_conn,
                         args=(conn, loop), daemon=True).start()


def handle_stub_conn(conn, loop):
    buf = ""
    try:
        while True:
            data = conn.recv(4096).decode()
            if not data:
                break
            buf += data
            while "\n" in buf:
                line, buf = buf.split("\n", 1)
                resp = handle_stub_message(line, loop)
                if resp:
                    conn.sendall(resp.encode())
    except Exception:
        pass
    finally:
        conn.close()

# ------------------------------------------------------------------ #
# HTTP server (serve panel/)                                            #
# ------------------------------------------------------------------ #

async def http_handler(request):
    path = request.path.lstrip("/") or "index.html"
    fpath = PANEL_DIR / path
    if not fpath.exists():
        return web.Response(status=404, text="Not found")
    content_types = {
        ".html": "text/html", ".css": "text/css",
        ".js":   "application/javascript",
    }
    ct = content_types.get(fpath.suffix, "application/octet-stream")
    return web.Response(body=fpath.read_bytes(), content_type=ct)


def _parse_json_or_query(request, body: dict | None = None) -> dict:
    data = dict(request.query)
    if body:
        data.update(body)
    return data


async def api_state(request):
    return web.json_response(state)


async def api_button(request):
    body = await request.json() if request.can_read_body else {}
    data = _parse_json_or_query(request, body)
    line = int(data.get("line", 17))
    value = bool(int(data.get("value", 1)))
    await set_button(line, value)
    return web.json_response({"ok": True, "line": line, "value": value})


async def api_button_press(request):
    body = await request.json() if request.can_read_body else {}
    data = _parse_json_or_query(request, body)
    line = int(data.get("line", 17))
    duration_ms = max(0, int(data.get("duration_ms", 150)))
    await press_button(line, duration_ms)
    return web.json_response({"ok": True, "line": line, "duration_ms": duration_ms})


async def api_rfid_tap(request):
    body = await request.json() if request.can_read_body else {}
    data = _parse_json_or_query(request, body)
    uid = data.get("uid", "04:AB:CD:EF:01:23")
    await tap_rfid(uid)
    return web.json_response({"ok": True, "uid": uid, "present": True})


async def api_rfid_remove(request):
    await remove_rfid()
    return web.json_response({"ok": True, "present": False})


async def api_range(request):
    body = await request.json() if request.can_read_body else {}
    data = _parse_json_or_query(request, body)
    mm = int(data.get("value", 300))
    await set_range(mm)
    return web.json_response({"ok": True, "value": mm})

# ------------------------------------------------------------------ #
# Main                                                                  #
# ------------------------------------------------------------------ #

async def main():
    loop = asyncio.get_running_loop()

    # Unix socket server in background thread
    threading.Thread(target=unix_server_thread, args=(loop,), daemon=True).start()

    # WebSocket server
    ws_server = await websockets.serve(ws_handler, "0.0.0.0", WS_PORT)
    print(f"[bridge] WebSocket  ws://0.0.0.0:{WS_PORT}")

    # HTTP server
    app = web.Application()
    app.router.add_route("GET",  "/api/state", api_state)
    app.router.add_route("POST", "/api/button", api_button)
    app.router.add_route("POST", "/api/button/press", api_button_press)
    app.router.add_route("POST", "/api/rfid/tap", api_rfid_tap)
    app.router.add_route("POST", "/api/rfid/remove", api_rfid_remove)
    app.router.add_route("POST", "/api/range", api_range)
    app.router.add_route("GET", "/{path_info:.*}", http_handler)
    runner = web.AppRunner(app)
    await runner.setup()
    site = web.TCPSite(runner, "0.0.0.0", HTTP_PORT)
    await site.start()
    print(f"[bridge] HTTP panel http://0.0.0.0:{HTTP_PORT}")

    await asyncio.Future()  # run forever


if __name__ == "__main__":
    asyncio.run(main())
