import os
import json
import socket
import simple_websocket
from datetime import datetime, timezone
from threading import Lock

from flask import Flask, jsonify, render_template, request, send_file
from flask_socketio import SocketIO, emit as sio_emit

app = Flask(__name__)
socketio = SocketIO(app, async_mode='threading', cors_allowed_origins='*')

ROOT_OFFLINE_TIMEOUT_S = 15

# O Flask é o SERVIDOR WebSocket; o ROOT (esp_websocket_client) é o CLIENTE e
# conecta em ws://<host>:5000 -> path "/". Servimos esse WS na MESMA porta 5000
# do HTTP/Socket.IO, via um dispatcher WSGI (ver _RootWSDispatch, no fim).
ROOT_WS_PATH = "/"

_lock = Lock()
_devices = {}
_ota_pending_url = None
_ota_pending_name = None
_firmware_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "firmware_upload.bin")
_root_ws = None            # simple_websocket.Server do ROOT conectado (ou None)
_root_ws_lock = Lock()     # serializa envios vindos de threads diferentes

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
    """Empurra um comando JSON ao ROOT pelo WebSocket. False se o ROOT está off
    ou se o envio falhar. Protegido por lock: vários endpoints/threads podem
    empurrar comandos (OTA/READ/RESET/MARKVALID/OTAMON) ao mesmo tempo."""
    ws = _root_ws
    if ws is None:
        return False
    try:
        with _root_ws_lock:
            ws.send(json.dumps(obj))
        return True
    except Exception:
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


def _root_ws_handler(environ):
    """Atende a conexão WebSocket crua do ROOT na MESMA porta 5000 (path "/").
    O ROOT é o cliente (esp_websocket_client); aqui o Flask é o servidor. A
    simple_websocket sequestra o socket no handshake; no fim devolvemos [] sem
    chamar start_response (padrão do modo Werkzeug, igual ao engineio)."""
    global _root_ws
    ws = simple_websocket.Server.accept(environ, ping_interval=25)
    _root_ws = ws
    print("[WS] ROOT conectado", flush=True)
    try:
        # Sincroniza o estado do monitor com o ROOT recém-conectado (cobre o ROOT
        # ter reiniciado, p.ex. após um self-update, perdendo o flag).
        with _lock:
            on = _ota_monitor_enabled
        with _root_ws_lock:
            ws.send(json.dumps({"cmd": "OTAMON", "on": on}))
        while ws.connected:
            if ws.receive() is None:   # bloqueia até chegar dado (ou a conexão cair)
                break
    except simple_websocket.ConnectionClosed:
        pass
    except Exception as e:
        print(f"[WS] erro na conexão do ROOT: {e}", flush=True)
    finally:
        if _root_ws is ws:
            _root_ws = None
        try:
            ws.close()
        except Exception:
            pass
        print("[WS] ROOT desconectado", flush=True)
    return []


class _RootWSDispatch:
    """Middleware WSGI na frente do Flask/Socket.IO: intercepta o upgrade do ROOT
    em ROOT_WS_PATH e entrega ao handler cru; todo o resto (HTTP /api/*, páginas,
    /socket.io/ do navegador) segue para o app normalmente, na mesma porta."""
    def __init__(self, wsgi_app):
        self.wsgi_app = wsgi_app

    def __call__(self, environ, start_response):
        if (environ.get("PATH_INFO", "/") == ROOT_WS_PATH
                and environ.get("HTTP_UPGRADE", "").lower() == "websocket"):
            return _root_ws_handler(environ)
        return self.wsgi_app(environ, start_response)


@socketio.on('read_request')
def handle_read_request():
    if not _push_to_root({"cmd": "READ"}):
        sio_emit('read_error', {'msg': 'Root não conectado'})


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
    cmd = {"cmd": "OTA", "file": fname, "url": ota_url}
    if target:
        cmd["target"] = target
    pushed = _push_to_root(cmd)

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


@app.post("/api/markvalid")
def mark_valid_node():
    """Pede ao ROOT para chamar esp_ota_mark_app_valid_cancel_rollback() no alvo.
    Se o alvo for o próprio ROOT, ele executa localmente; senão repassa por unicast
    na mesh (BIN_MSG_MARK_VALID). Fire-and-forget, como o RESET."""
    payload = request.get_json(silent=True) or {}
    target = (payload.get("target") or "").strip().lower()
    if not target:
        return jsonify({"ok": False, "error": "missing target"}), 400
    pushed = _push_to_root({"cmd": "MARKVALID", "target": target})
    return jsonify({"ok": True, "target": target, "pushed": pushed})


@app.get("/")
def index():
    return render_template("index.html")


@app.get("/topology")
def topology():
    return render_template("mesh_tree.html")


# Coloca o servidor WS cru na frente do Flask/Socket.IO, na mesma porta 5000.
# (Tem de ser depois de SocketIO(app), que já embrulhou app.wsgi_app.)
app.wsgi_app = _RootWSDispatch(app.wsgi_app)

if __name__ == "__main__":
    socketio.start_background_task(_watch_root)
    socketio.run(app, host="0.0.0.0", port=5000, debug=False)
