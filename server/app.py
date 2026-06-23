import os
import json
import socket
from datetime import datetime, timezone
from threading import Lock

import paho.mqtt.client as mqtt
from flask import Flask, jsonify, render_template, request, send_file
from flask_socketio import SocketIO, emit as sio_emit

app = Flask(__name__)
socketio = SocketIO(app, async_mode='threading', cors_allowed_origins='*')

# ── Broker MQTT (mesmo Raspberry do Flask). IP fixo, sem auth. ────────────────
BROKER_HOST = "localhost"
BROKER_PORT = 1883

# Tópicos — ver docs/superpowers/specs/2026-06-18-i2c-to-mqtt-migration-design.md
T_READING      = "mesh/reading"
T_STATUS       = "mesh/status"
T_OFFLINE      = "mesh/offline"
T_OTA_PROGRESS = "mesh/ota/progress"
T_ROOT_STATE   = "mesh/root/state"
T_CMD_READ      = "mesh/cmd/read"
T_CMD_OTAMON    = "mesh/cmd/otamon"
T_CMD_RESET     = "mesh/cmd/reset"
T_CMD_MARKVALID = "mesh/cmd/markvalid"
T_CMD_OTA       = "mesh/cmd/ota"

DEVICE_OFFLINE_TIMEOUT_S = 90   # 90 s (3 polls de status de 30 s) sem mensagem -> offline

_lock = Lock()
_devices = {}
_firmware_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "firmware_upload.bin")
_root_online = False          # vem do mesh/root/state (retained + LWT)
_mqtt = None

# ── Monitor OTA (inalterado em relação a hoje) ────────────────────────────────
_ota_monitor_enabled = False
_ota_state = {
    "active": False, "op": None, "file": None, "total": 0, "pct": 0,
    "started": None, "finished": None,
    "targets": [],
    "self_status": None,
}
_ota_history = []
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


def _is_fresh(last_seen_iso: str) -> bool:
    # Liveness por timeout: o root pede status a cada 30 s e o driver responde
    # (mesh/status). Se nada chegar em DEVICE_OFFLINE_TIMEOUT_S (90 s = 3 polls)
    # o device é tratado como offline. Margem folgada evita falso-offline por um
    # poll atrasado/perdido (não derruba a malha inteira de uma vez).
    try:
        ts = datetime.fromisoformat(last_seen_iso.replace("Z", "+00:00"))
        return (datetime.now(timezone.utc) - ts).total_seconds() <= DEVICE_OFFLINE_TIMEOUT_S
    except Exception:
        return False


def _device_online(info: dict, root_online: bool) -> bool:
    # Online = visto (status/leitura) dentro do timeout E sem sinal de offline.
    # online_flag: True via _apply_status; False via mesh/offline (sinal imediato
    # no CHILD_DISCONNECTED). Offline também vem do timeout de last_seen.
    if info.get("layer") == 1:
        return root_online
    if not root_online:
        return False
    return bool(info.get("online", False)) and _is_fresh(info.get("last_seen", ""))


def _get_state():
    with _lock:
        root_online = _root_online
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


# ── Aplicadores de telemetria (compartilhados pelos callbacks MQTT) ───────────
def _apply_status(payload: dict):
    mac = (payload.get("mac") or "").strip().lower()
    if not mac:
        return
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


def _map_reading(payload: dict):
    """Mapeia o payload MQTT {mac,ch1,ch2,ch3,tensao} para o formato que o
    navegador já consome (CH1/CH2/CH3), preservando 'tensao'. Função pura:
    sem ts, sem efeitos — testável isoladamente."""
    mac = (payload.get("mac") or "").strip().lower()
    if not mac:
        return None
    return {
        "mac": mac,
        "CH1": payload.get("ch1"),
        "CH2": payload.get("ch2"),
        "CH3": payload.get("ch3"),
        "tensao": payload.get("tensao"),
    }


def _apply_ota_event(ev: dict):
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
                for t in _ota_state["targets"]:
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


# ── Publicação de comandos ────────────────────────────────────────────────────
def _publish(topic: str, obj: dict, retain: bool = False) -> bool:
    if _mqtt is None:
        return False
    try:
        info = _mqtt.publish(topic, json.dumps(obj), qos=1, retain=retain)
        return info.rc == mqtt.MQTT_ERR_SUCCESS
    except Exception:
        return False


# ── Callbacks MQTT ────────────────────────────────────────────────────────────
def _on_connect(client, userdata, flags, reason_code, properties=None):
    for t in (T_READING, T_STATUS, T_OFFLINE, T_OTA_PROGRESS, T_ROOT_STATE):
        client.subscribe(t, qos=1)
    # Reflete o estado atual do monitor (retained) para o ROOT recém-conectado.
    client.publish(T_CMD_OTAMON, json.dumps({"on": _ota_monitor_enabled}), qos=1, retain=True)
    print("[MQTT] conectado ao broker", flush=True)


def _on_message(client, userdata, msg):
    global _root_online
    try:
        payload = json.loads(msg.payload.decode("utf-8") or "{}")
    except Exception:
        payload = {}
    topic = msg.topic

    if topic == T_STATUS:
        _apply_status(payload)
        socketio.emit('state_update', _get_state())
    elif topic == T_READING:
        ev = _map_reading(payload)
        if ev:
            ev["ts"] = datetime.now(timezone.utc).isoformat(timespec="seconds")
            socketio.emit('reading_update', ev)
    elif topic == T_OFFLINE:
        mac = (payload.get("mac") or "").strip().lower()
        if mac:
            with _lock:
                if mac in _devices:
                    _devices[mac]["online"] = False
            socketio.emit('state_update', _get_state())
    elif topic == T_OTA_PROGRESS:
        _apply_ota_event(payload)
        _emit_ota()
    elif topic == T_ROOT_STATE:
        _root_online = bool(payload.get("online"))
        socketio.emit('state_update', _get_state())


def _watch_root():
    prev_online = {}
    while True:
        socketio.sleep(1)
        with _lock:
            snapshot = dict(_devices)
        curr_online = {mac: _device_online(info, _root_online) for mac, info in snapshot.items()}
        if curr_online != prev_online:
            socketio.emit('state_update', _get_state())
            prev_online = curr_online


# ── Socket.IO (navegador) ─────────────────────────────────────────────────────
@socketio.on('read_request')
def handle_read_request():
    if not _publish(T_CMD_READ, {}):
        sio_emit('read_error', {'msg': 'Broker indisponível'})


@socketio.on('set_ota_monitor')
def handle_set_ota_monitor(data):
    global _ota_monitor_enabled
    on = bool((data or {}).get('on'))
    with _lock:
        _ota_monitor_enabled = on
    pushed = _publish(T_CMD_OTAMON, {"on": on}, retain=True)
    socketio.emit('ota_monitor_state', {"monitor_enabled": on, "pushed": pushed})


# ── REST (mantidos) ───────────────────────────────────────────────────────────
@app.post("/api/ota/upload")
def upload_firmware():
    if "file" not in request.files:
        return jsonify({"ok": False, "error": "no file attached"}), 400
    f = request.files["file"]
    if not f.filename.lower().endswith(".bin"):
        return jsonify({"ok": False, "error": "o arquivo deve ser .bin"}), 400
    fname = os.path.basename(f.filename)
    target = (request.form.get("target") or "").strip().lower()
    f.save(_firmware_path)
    size = os.path.getsize(_firmware_path)
    port = request.host.split(":")[1] if ":" in request.host else "5000"
    ota_url = f"http://{_get_local_ip()}:{port}/firmware/latest.bin"

    cmd = {"file": fname, "url": ota_url}
    if target:
        cmd["target"] = target
    pushed = _publish(T_CMD_OTA, cmd)

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


@app.get("/api/ota/state")
def ota_state():
    return jsonify(_ota_payload())


@app.post("/api/reset")
def reset_node():
    payload = request.get_json(silent=True) or {}
    target = (payload.get("target") or "").strip().lower()
    if not target:
        return jsonify({"ok": False, "error": "missing target"}), 400
    pushed = _publish(T_CMD_RESET, {"target": target})
    return jsonify({"ok": True, "target": target, "pushed": pushed})


@app.post("/api/markvalid")
def mark_valid_node():
    payload = request.get_json(silent=True) or {}
    target = (payload.get("target") or "").strip().lower()
    if not target:
        return jsonify({"ok": False, "error": "missing target"}), 400
    pushed = _publish(T_CMD_MARKVALID, {"target": target})
    return jsonify({"ok": True, "target": target, "pushed": pushed})


@app.get("/")
def index():
    return render_template("index.html")


@app.get("/topology")
def topology():
    return render_template("mesh_tree.html")


if __name__ == "__main__":
    _mqtt = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    _mqtt.on_connect = _on_connect
    _mqtt.on_message = _on_message
    _mqtt.loop_start()
    # connect_async + loop_start: não bloqueia nem quebra se o broker ainda não
    # subiu; o paho (re)conecta sozinho quando o Mosquitto ficar disponível.
    _mqtt.connect_async(BROKER_HOST, BROKER_PORT, keepalive=30)
    socketio.start_background_task(_watch_root)
    socketio.run(app, host="0.0.0.0", port=5000, debug=False)
