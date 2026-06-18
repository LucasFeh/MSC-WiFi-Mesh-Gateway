# MQTT Migration — FLASK (plan 2 of 3) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make Flask an MQTT client: consume telemetry from the broker (updating `_devices`/`_ota_state` and bridging to the browser via Socket.IO), publish commands to `mesh/cmd/*`, and remove the raw root WebSocket server + the HTTP telemetry endpoints.

**Architecture:** A `paho-mqtt` client connects to the broker on the Raspberry, subscribes to `mesh/reading|status|offline|ota/progress|root/state`, and dispatches each message into the existing in-memory state + `socketio.emit(...)`. Commands from the dashboard/REST publish to `mesh/cmd/*`. Root presence comes from the retained/LWT `mesh/root/state`. The browser Socket.IO layer and the OTA state machine are unchanged; only the transport changes.

**Tech Stack:** Python 3.11+, Flask, Flask-SocketIO (threading mode), `paho-mqtt` v2.

**Cross-plan note:** Contract = `docs/superpowers/specs/2026-06-18-i2c-to-mqtt-migration-design.md`. Test against a Mosquitto broker; the ROOT plan can be running, or use `mosquitto_pub` to simulate telemetry.

---

## File Structure

- Modify `server/requirements.txt` — add `paho-mqtt`.
- Modify `server/app.py` — full rewrite of the transport: add MQTT client + callbacks; replace `_push_to_root` with `_publish`; remove `_RootWSDispatch`/`_root_ws_handler`/`simple_websocket`/the wsgi wrap and the HTTP telemetry endpoints; presence via `_root_online`.
- Create `server/test_app_mapping.py` — unit test for the pure reading-mapping helper.

The browser assets (`static/`, `templates/`) are unchanged: `reading_update` keeps the `CH1/CH2/CH3` shape (now plus `tensao`), and `state_update`/`ota_progress` are emitted exactly as today.

---

## Task 1: Add paho-mqtt dependency

**Files:**
- Modify: `server/requirements.txt`

- [ ] **Step 1: Add the dependency**

Replace `server/requirements.txt` with:
```
Flask>=3.0
Flask-SocketIO>=5.0
websockets>=10.0
paho-mqtt>=2.0
```

- [ ] **Step 2: Install**

Run (in the Pi's venv): `pip install -r server/requirements.txt`
Expected: `paho-mqtt` installs.

- [ ] **Step 3: Commit**

```bash
git add server/requirements.txt
git commit -m "build(flask): add paho-mqtt dependency"
```

---

## Task 2: Rewrite `server/app.py` over MQTT

**Files:**
- Modify: `server/app.py`

- [ ] **Step 1: Replace the entire file with the MQTT version**

```python
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
BROKER_HOST = "192.168.15.191"
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

ROOT_OFFLINE_TIMEOUT_S = 15

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
    return _is_fresh(info.get("last_seen", ""))


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
    _mqtt.connect(BROKER_HOST, BROKER_PORT, keepalive=30)
    _mqtt.loop_start()
    socketio.start_background_task(_watch_root)
    socketio.run(app, host="0.0.0.0", port=5000, debug=False)
```

- [ ] **Step 2: Sanity check (imports + no leftover WS refs)**

Run:
```
python -c "import ast,sys; ast.parse(open('server/app.py').read()); print('ok')"
grep -n "simple_websocket\|_RootWSDispatch\|_root_ws\|_push_to_root\|/api/status\|/api/reading\|/api/offline\|/api/ota/progress" server/app.py
```
Expected: `ok`, and the grep returns **nothing** (all WS/HTTP-telemetry code removed).

- [ ] **Step 3: Commit**

```bash
git add server/app.py
git commit -m "feat(flask): consumir telemetria e publicar comandos via MQTT (remove WS+HTTP telemetria)"
```

---

## Task 3: Unit test the reading mapping

**Files:**
- Create: `server/test_app_mapping.py`

- [ ] **Step 1: Write the test**

```python
from app import _map_reading


def test_map_reading_translates_channels_and_keeps_tensao():
    out = _map_reading({"mac": "AA:BB:CC:DD:EE:01", "ch1": 10, "ch2": 20, "ch3": 30, "tensao": 3.30})
    assert out == {"mac": "aa:bb:cc:dd:ee:01", "CH1": 10, "CH2": 20, "CH3": 30, "tensao": 3.30}


def test_map_reading_returns_none_without_mac():
    assert _map_reading({"ch1": 1}) is None
```

- [ ] **Step 2: Run it**

Run: `cd server && python -m pytest test_app_mapping.py -v`
Expected: 2 passed. (Importing `app` does not start the server because the MQTT/socketio startup is under `if __name__ == "__main__"`.)

- [ ] **Step 3: Commit**

```bash
git add server/test_app_mapping.py
git commit -m "test(flask): cobre o mapeamento de leitura MQTT->browser"
```

---

## Task 4: Integration test against the broker

**Files:** none (runtime verification)

- [ ] **Step 1: Start broker + Flask**

On the Pi: `sudo systemctl start mosquitto`; then `cd server && python app.py`.
Expected: console prints `[MQTT] conectado ao broker`.

- [ ] **Step 2: Open the dashboard and simulate telemetry**

Open `http://<pi>:5000/`. In another shell:
```
mosquitto_pub -h 192.168.15.191 -t mesh/root/state -r -m '{"online":true}'
mosquitto_pub -h 192.168.15.191 -t mesh/status  -m '{"mac":"aa:bb:cc:dd:ee:00","parent":"","layer":1,"rssi":-40,"version":"2-Gateway"}'
mosquitto_pub -h 192.168.15.191 -t mesh/reading -m '{"mac":"aa:bb:cc:dd:ee:01","ch1":1,"ch2":2,"ch3":3,"tensao":3.30}'
```
Expected: the dashboard shows the root online, a device row, and a reading update.

- [ ] **Step 3: Verify commands are published**

Subscribe: `mosquitto_sub -h 192.168.15.191 -t 'mesh/cmd/#' -v`.
In the dashboard, click the "read" button and toggle the OTA monitor.
Expected: `mesh/cmd/read {}` and a **retained** `mesh/cmd/otamon {"on":true}` appear.

- [ ] **Step 4: Verify presence via LWT**

`mosquitto_pub -h 192.168.15.191 -t mesh/root/state -r -m '{"online":false}'`
Expected: dashboard marks the root (and dependent nodes) offline.

---

## Self-Review

- **Spec coverage (FLASK rows):** subscribes to `mesh/reading|status|offline|ota/progress|root/state` (Task 2 `_on_connect`/`_on_message`); publishes `mesh/cmd/read|otamon|reset|markvalid|ota` (Task 2 handlers/REST); presence via `mesh/root/state` → `_root_online` (Task 2). Removed WS server + `/api/status|reading|offline|ota/progress` (Task 2, verified in Step 2). Kept `/api/ota/upload`, `/firmware/latest.bin`, Socket.IO, OTA state machine. Gap: none.
- **Placeholder scan:** none — full file + full test provided.
- **Type consistency:** topics `T_*` defined once and reused; `_map_reading` output keys (`CH1/CH2/CH3/tensao`) match the test in Task 3 and the browser's existing `reading_update` consumer; `_apply_ota_event` preserves the exact `_ota_state` shape used by `_ota_payload`/the dashboard.
