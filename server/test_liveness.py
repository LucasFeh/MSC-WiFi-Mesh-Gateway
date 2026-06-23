"""
Testes da liveness do Flask no modelo POLLED (request/response).

Design: docs/superpowers/specs/2026-06-23-status-polled-request-response-design.md

Status chega a cada 30 s (root pede, driver responde). Online/offline =
frescor de last_seen (timeout 90 s) + offline imediato via mesh/offline.
Não há mais mesh/members nem flip em massa por snapshot de routing table.

Estes testes isolam a lógica pura de app.py (sem mesh, sem broker). Rodam com
pytest no Raspberry/CI (deps reais) ou via scratchpad/run_liveness.py (stubs).
"""

from datetime import datetime, timezone, timedelta

import app
from app import _device_online

STATUS_POLL_S = 30   # cadência do status no firmware (status_timer do root)


def _iso_ago(seconds: int) -> str:
    return (datetime.now(timezone.utc) - timedelta(seconds=seconds)).isoformat(timespec="seconds")


# ── Frescor básico ────────────────────────────────────────────────────────────

def test_status_fresco_fica_online():
    info = {"layer": 2, "online": True, "last_seen": _iso_ago(10)}
    assert _device_online(info, True) is True


def test_silencio_acima_do_timeout_fica_offline():
    info = {"layer": 2, "online": True, "last_seen": _iso_ago(95)}
    assert _device_online(info, True) is False


def test_root_layer1_segue_root_online_ignora_last_seen():
    info = {"layer": 1, "online": False, "last_seen": _iso_ago(9999)}
    assert _device_online(info, True) is True
    assert _device_online(info, False) is False


# ── O conserto do flip em massa (problema #1) ─────────────────────────────────
# Com status a cada 30 s, perder 1–2 polls (silêncio de 35–65 s) NÃO pode
# derrubar o device. O bug antigo era timeout 25 s < cadência -> um atraso
# expirava todo mundo junto.

def test_um_poll_de_status_perdido_nao_derruba():
    info = {"layer": 2, "online": True, "last_seen": _iso_ago(STATUS_POLL_S + 5)}  # 35 s
    assert _device_online(info, True) is True


def test_dois_polls_de_status_perdidos_ainda_nao_derrubam():
    info = {"layer": 2, "online": True, "last_seen": _iso_ago(2 * STATUS_POLL_S + 5)}  # 65 s
    assert _device_online(info, True) is True


def test_timeout_tem_margem_confortavel_sobre_a_cadencia():
    """Tolera >=2 polls perdidos: o oposto do bug (timeout 25 s vs cadência)."""
    assert app.DEVICE_OFFLINE_TIMEOUT_S >= 2 * STATUS_POLL_S


# ── Offline rápido via mesh/offline ───────────────────────────────────────────

def test_mesh_offline_forca_offline_mesmo_fresco():
    # T_OFFLINE seta online=False; deve vencer o frescor de last_seen.
    info = {"layer": 2, "online": False, "last_seen": _iso_ago(2)}
    assert _device_online(info, True) is False


# ── _apply_status marca vivo (o status polled é o heartbeat) ───────────────────

def test_apply_status_marca_online_e_atualiza_last_seen():
    app._devices.clear()
    app._apply_status({"mac": "AA:BB:CC:00:00:01", "layer": 2, "rssi": -55,
                       "version": "3-CH1", "parent": "aa:bb:cc:00:00:00"})
    info = app._devices["aa:bb:cc:00:00:01"]
    assert info["online"] is True
    assert _device_online(info, True) is True


# ── Reconhecimento de nó pela LEITURA (robustez) ──────────────────────────────
# Entradas de nó não vêm mais de mesh/members. Para o Flask reconhecer um nó que
# responde (mesmo só leitura, ou antes do 1º status), a leitura também registra e
# mantém o device vivo.

def test_leitura_registra_no_novo_online():
    app._devices.clear()
    app._touch_device("AA:BB:CC:00:00:09")
    info = app._devices["aa:bb:cc:00:00:09"]
    assert info["online"] is True
    assert _device_online(info, True) is True


def test_leitura_refresca_e_revive_no_existente():
    app._devices.clear()
    app._devices["aa:bb:cc:00:00:09"] = {"layer": 2, "rssi": -60, "version": "3",
                                         "parent": "", "hits": 0,
                                         "online": False, "last_seen": _iso_ago(300)}
    app._touch_device("aa:bb:cc:00:00:09")
    info = app._devices["aa:bb:cc:00:00:09"]
    assert info["layer"] == 2          # não apaga telemetria já conhecida
    assert _device_online(info, True) is True
