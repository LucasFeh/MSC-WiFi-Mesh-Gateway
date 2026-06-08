import os
import json
import socket
import asyncio
import threading
import websockets
from datetime import datetime, timezone
from threading import Lock

from flask import Flask, jsonify, render_template, request, send_file
from flask_socketio import SocketIO, emit as sio_emit

app = Flask(__name__)
socketio = SocketIO(app, async_mode='threading', cors_allowed_origins='*')

ROOT_OFFLINE_TIMEOUT_S = 15

_lock = Lock()
_devices = {}
_ota_pending_url = None
_ota_pending_name = None
_firmware_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "firmware_upload.bin")
_root_ws = None
_ws_loop = None

# ── Monitor OTA ──────────────────────────────────────────────────────────────
# Telemetria do ROOT durante o OTA. Desligada por padrão (o ROOT só reporta
# quando ligada por botão, evitando overhead). Tudo em memória, como _devices.
_ota_monitor_enabled = False
_ota_state = {
    "active": False, "op": None, "file": None, "total": 0, "pct": 0,
    "started": None, "finished": None,
    "targets": [],        # [{"mac":.., "status":"pending"|"ok"|"fail"|"timeout"}]
    "self_status": None,  # "running"|"rebooting"|"fail" (op self)
}
_ota_history = []         # últimos 10: {"file","op","finished","ok","fail","timeout","self_status"}
_OTA_HISTORY_MAX = 10


def _get_local_ip() -> str:
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except Exception:
        return "127.0.0.1"


def _is_root_online(last_seen_iso: str) -> bool:
    try:
        ts = datetime.fromisoformat(last_seen_iso.replace("Z", "+00:00"))
        age = (datetime.now(timezone.utc) - ts).total_seconds()
        return age <= ROOT_OFFLINE_TIMEOUT_S
    except Exception:
        return False


def _device_online(info: dict, root_online: bool) -> bool:
    if info.get("layer") == 1:
        return root_online
    if not root_online:
        return False
    return _is_root_online(info.get("last_seen", ""))


def _get_state():
    with _lock:
        root = next((info for info in _devices.values() if info.get("layer") == 1), None)
        root_online = _is_root_online(root.get("last_seen", "")) if root else False
        return {
            "devices": sorted(
                [{"mac": mac, **info, "online": _device_online(info, root_online)} for mac, info in _devices.items()],
                key=lambda d: (not d["online"], d["mac"])
            )
        }


def _ota_payload():
    with _lock:
        return {
            "monitor_enabled": _ota_monitor_enabled,
            "state": {**_ota_state, "targets": [dict(t) for t in _ota_state["targets"]]},
            "history": [dict(h) for h in _ota_history],
        }


def _emit_ota():
    socketio.emit('ota_progress', _ota_payload())


def _push_to_root(obj: dict) -> bool:
    """Empurra um comando JSON ao ROOT pelo WebSocket. False se o ROOT está off."""
    if _root_ws is not None and _ws_loop is not None:
        asyncio.run_coroutine_threadsafe(_root_ws.send(json.dumps(obj)), _ws_loop)
        return True
    return False


def _watch_root():
    prev_online = {}
    while True:
        socketio.sleep(1)
        with _lock:
            snapshot = dict(_devices)
        root = next((info for info in snapshot.values() if info.get("layer") == 1), None)
        root_online = _is_root_online(root.get("last_seen", "")) if root else False
        curr_online = {mac: _device_online(info, root_online) for mac, info in snapshot.items()}
        if curr_online != prev_online:
            socketio.emit('state_update', _get_state())
            prev_online = curr_online


async def _ws_root_handler(websocket):
    global _root_ws
    _root_ws = websocket
    try:
        # Re-sincroniza o estado do monitor com o ROOT recém-conectado (cobre o
        # ROOT ter reiniciado, p.ex. após um self-update, perdendo o flag).
        with _lock:
            on = _ota_monitor_enabled
        await websocket.send(json.dumps({"cmd": "OTAMON", "on": on}))
        async for _ in websocket:
            pass
    finally:
        _root_ws = None


async def _ws_server():
    async with websockets.serve(_ws_root_handler, "0.0.0.0", 5001):
        await asyncio.Future()


def _start_ws_thread():
    global _ws_loop
    _ws_loop = asyncio.new_event_loop()
    asyncio.set_event_loop(_ws_loop)
    _ws_loop.run_until_complete(_ws_server())


@socketio.on('read_request')
def handle_read_request():
    global _root_ws, _ws_loop
    if _root_ws is None or _ws_loop is None:
        sio_emit('read_error', {'msg': 'Root não conectado'})
        return
    asyncio.run_coroutine_threadsafe(_root_ws.send('{"cmd":"READ"}'), _ws_loop)


@socketio.on('set_ota_monitor')
def handle_set_ota_monitor(data):
    """Liga/desliga a telemetria OTA: atualiza o flag, avisa o ROOT (OTAMON) e
    reemite o novo estado a todos os navegadores."""
    global _ota_monitor_enabled
    on = bool((data or {}).get('on'))
    with _lock:
        _ota_monitor_enabled = on
    pushed = _push_to_root({"cmd": "OTAMON", "on": on})
    socketio.emit('ota_monitor_state', {"monitor_enabled": on, "pushed": pushed})


@app.post("/api/status")
def receive_status():
    payload = request.get_json(silent=True) or {}
    mac = (payload.get("mac") or "").strip().lower()
    if not mac:
        return jsonify({"ok": False, "error": "missing mac"}), 400
    with _lock:
        existing = _devices.get(mac, {})
        _devices[mac] = {
            "layer":     payload.get("layer", existing.get("layer")),
            "rssi":      payload.get("rssi", existing.get("rssi")),
            "version":   payload.get("version", existing.get("version")),
            "parent":    (payload.get("parent") or existing.get("parent") or "").strip().lower(),
            "hits":      existing.get("hits", 0) + 1,
            "last_seen": datetime.now(timezone.utc).isoformat(timespec="seconds"),
            "online":    True,
        }
    socketio.emit('state_update', _get_state())
    return jsonify({"ok": True})


@app.post("/api/reading")
def receive_reading():
    payload = request.get_json(silent=True) or {}
    mac = (payload.get("mac") or "").strip().lower()
    if not mac:
        return jsonify({"ok": False, "error": "missing mac"}), 400
    reading = {
        "mac": mac,
        "CH1": payload.get("CH1"),
        "CH2": payload.get("CH2"),
        "CH3": payload.get("CH3"),
        "ts":  datetime.now(timezone.utc).isoformat(timespec="seconds"),
    }
    socketio.emit('reading_update', reading)
    return jsonify({"ok": True})


@app.post("/api/offline")
def receive_offline():
    payload = request.get_json(silent=True) or {}
    mac = (payload.get("mac") or "").strip().lower()
    if not mac:
        return jsonify({"ok": False, "error": "missing mac"}), 400
    with _lock:
        if mac in _devices:
            _devices[mac]["online"] = False
    socketio.emit('state_update', _get_state())
    return jsonify({"ok": True})


@app.post("/api/ota/upload")
def upload_firmware():
    global _ota_pending_url, _ota_pending_name
    if "file" not in request.files:
        return jsonify({"ok": False, "error": "no file attached"}), 400
    f = request.files["file"]
    if not f.filename.lower().endswith(".bin"):
        return jsonify({"ok": False, "error": "o arquivo deve ser .bin"}), 400
    # Preserva o NOME original (ex.: "Gateway.bin", "Driver-1.bin") — é ele que o
    # ROOT usa para rotear (self-update vs repasse via mesh).
    fname = os.path.basename(f.filename)
    # Alvo unicast opcional: se presente, o ROOT envia o .bin só para esse MAC,
    # ignorando o roteamento por nome. Ausente -> broadcast por nome (atual).
    target = (request.form.get("target") or "").strip().lower()
    f.save(_firmware_path)
    size = os.path.getsize(_firmware_path)
    port = request.host.split(":")[1] if ":" in request.host else "5000"
    ota_url = f"http://{_get_local_ip()}:{port}/firmware/latest.bin"
    with _lock:
        _ota_pending_url = ota_url
        _ota_pending_name = fname

    # Empurra o comando OTA ao ROOT pelo WebSocket (mesmo canal do READ). O ROOT
    # decide a rota pelo nome (ou unicast, se 'target' vier) e baixa o .bin de ota_url.
    pushed = False
    if _root_ws is not None and _ws_loop is not None:
        cmd = {"cmd": "OTA", "file": fname, "url": ota_url}
        if target:
            cmd["target"] = target
        msg = json.dumps(cmd)
        asyncio.run_coroutine_threadsafe(_root_ws.send(msg), _ws_loop)
        pushed = True

    return jsonify({"ok": True, "fw_url": ota_url, "size": size,
                    "file": fname, "target": target or None, "pushed": pushed})


@app.get("/firmware/latest.bin")
def serve_firmware():
    if not os.path.exists(_firmware_path):
        return jsonify({"error": "nenhum firmware enviado ainda"}), 404
    return send_file(_firmware_path, mimetype="application/octet-stream",
                     as_attachment=True, download_name="firmware.bin")


@app.get("/api/state")
def state():
    return jsonify(_get_state())


@app.post("/api/ota/progress")
def ota_progress():
    """Recebe os eventos de telemetria do ROOT (start/progress/ack/done),
    aplica em _ota_state, fecha no histórico no 'done', e reemite ao navegador."""
    ev = request.get_json(silent=True) or {}
    etype = ev.get("event")
    now = datetime.now(timezone.utc).isoformat(timespec="seconds")
    with _lock:
        if etype == "start":
            op = ev.get("op")
            _ota_state.update({
                "active": True,
                "op": op,
                "file": ev.get("file"),
                "total": ev.get("total", 0),
                "pct": 0,
                "started": now,
                "finished": None,
                "self_status": "running" if op == "self" else None,
                "targets": [{"mac": (m or "").lower(), "status": "pending"}
                            for m in (ev.get("targets") or [])],
            })
        elif etype == "progress":
            _ota_state["pct"] = ev.get("pct", _ota_state["pct"])
        elif etype == "ack":
            mac = (ev.get("mac") or "").lower()
            new_status = "ok" if ev.get("status") == "ok" else "fail"
            for t in _ota_state["targets"]:
                if t["mac"] == mac:
                    t["status"] = new_status
                    break
        elif etype == "done":
            _ota_state["active"] = False
            _ota_state["finished"] = now
            if _ota_state["op"] == "self":
                _ota_state["self_status"] = ev.get("status", "done")
                if ev.get("status") == "rebooting":
                    _ota_state["pct"] = 100
            else:
                for t in _ota_state["targets"]:        # pendentes => timeout
                    if t["status"] == "pending":
                        t["status"] = "timeout"
            ok = sum(1 for t in _ota_state["targets"] if t["status"] == "ok")
            fail = sum(1 for t in _ota_state["targets"] if t["status"] == "fail")
            timeout = sum(1 for t in _ota_state["targets"] if t["status"] == "timeout")
            _ota_history.insert(0, {
                "file": _ota_state["file"], "op": _ota_state["op"], "finished": now,
                "ok": ok, "fail": fail, "timeout": timeout,
                "self_status": _ota_state["self_status"],
            })
            del _ota_history[_OTA_HISTORY_MAX:]
    _emit_ota()
    return jsonify({"ok": True})


@app.get("/api/ota/state")
def ota_state():
    return jsonify(_ota_payload())


@app.post("/api/reset")
def reset_node():
    payload = request.get_json(silent=True) or {}
    target = (payload.get("target") or "").strip().lower()
    if not target:
        return jsonify({"ok": False, "error": "missing target"}), 400
    pushed = _push_to_root({"cmd": "RESET", "target": target})
    return jsonify({"ok": True, "target": target, "pushed": pushed})


@app.get("/")
def index():
    return render_template("index.html")


@app.get("/topology")
def topology():
    return render_template("mesh_tree.html")


if __name__ == "__main__":
    threading.Thread(target=_start_ws_thread, daemon=True).start()
    socketio.start_background_task(_watch_root)
    socketio.run(app, host="0.0.0.0", port=5000, debug=False)
