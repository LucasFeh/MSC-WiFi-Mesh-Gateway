"""
reading_watchdog.py
-------------------
Roda no Raspberry Pi junto ao broker MQTT.

Lógica:
  - Escuta mesh/cmd/read  → marca início de um novo ciclo de leitura.
  - Escuta mesh/reading   → registra quais MACs responderam no ciclo atual.
  - Escuta mesh/status    → descobre automaticamente os MACs conhecidos na mesh.
  - Ao receber mesh/cmd/read, aguarda RESPONSE_WINDOW_S segundos e verifica
    quais MACs conhecidos NÃO responderam neste ciclo.
  - Se um MAC acumular MISS_THRESHOLD ciclos consecutivos sem responder,
    publica uma leitura sintética zerada em mesh/reading para ele.

Uso:
    python reading_watchdog.py [--broker HOST] [--port PORT] [--macs MAC1 MAC2 ...]

Exemplos:
    python3 reading_watchdog.py --broker localhost
    python reading_watchdog.py
    python reading_watchdog.py --macs 38:18:2b:8e:75:c4 aa:bb:cc:dd:ee:ff
    python reading_watchdog.py --broker 192.168.10.190 --port 1883
"""

import argparse
import json
import logging
import threading
import time
from typing import Dict, Set

import paho.mqtt.client as mqtt

# ── Configurações padrão ──────────────────────────────────────────────────────
DEFAULT_BROKER_HOST   = "localhost"
DEFAULT_BROKER_PORT   = 1883

# Segundos que o watchdog espera respostas após um mesh/cmd/read antes de
# contabilizar quem ficou ausente naquele ciclo.
RESPONSE_WINDOW_S     = 10

# Quantos ciclos consecutivos sem resposta disparam a leitura sintética.
MISS_THRESHOLD        = 10

# Tópicos (mesmos do app.py)
T_READING  = "mesh/reading"
T_STATUS   = "mesh/status"
T_CMD_READ = "mesh/cmd/read"

# ── Estado compartilhado (protegido por _lock) ────────────────────────────────
_lock              = threading.Lock()
_known_macs: Set[str] = set()          # MACs descobertos via status/readings
_responded_this_cycle: Set[str] = set()  # MACs que responderam no ciclo atual
_miss_count: Dict[str, int] = {}       # ciclos consecutivos sem resposta por MAC
_cycle_active      = False             # True enquanto janela de resposta está aberta
_mqtt_client: mqtt.Client | None = None

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [watchdog] %(levelname)s %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S",
)
log = logging.getLogger("watchdog")


# ── Helpers ───────────────────────────────────────────────────────────────────

def _publish_synthetic(mac: str) -> None:
    """Publica leitura zerada para o MAC especificado em mesh/reading."""
    if _mqtt_client is None:
        return
    payload = {
        "mac":    mac.upper(),
        "ch1":    0,
        "ch2":    0,
        "ch3":    0,
        "tensao": 0.00,
    }
    _mqtt_client.publish(T_READING, json.dumps(payload), qos=1)
    log.warning("Leitura sintética publicada para %s (>= %d ciclos sem retorno)",
                mac, MISS_THRESHOLD)


def _close_cycle() -> None:
    """Chamado após RESPONSE_WINDOW_S segundos: contabiliza ausentes e dispara
    leituras sintéticas quando necessário."""
    with _lock:
        global _cycle_active
        if not _cycle_active:
            return
        _cycle_active = False

        absent = _known_macs - _responded_this_cycle
        present = _known_macs & _responded_this_cycle

        # Zera contagem de quem respondeu neste ciclo
        for mac in present:
            _miss_count[mac] = 0

        # Incrementa contagem de quem não respondeu
        for mac in absent:
            _miss_count[mac] = _miss_count.get(mac, 0) + 1
            log.info("%-20s  ausente  (miss=%d/%d)",
                     mac, _miss_count[mac], MISS_THRESHOLD)

        # Publica leitura sintética para MACs que atingiram o limite
        for mac in absent:
            if _miss_count[mac] >= MISS_THRESHOLD:
                _publish_synthetic(mac)

        _responded_this_cycle.clear()

        if absent:
            log.debug("Ciclo encerrado | presentes=%s | ausentes=%s",
                      sorted(present), sorted(absent))


# ── Callbacks MQTT ────────────────────────────────────────────────────────────

def _on_connect(client, userdata, flags, reason_code, properties=None):
    for topic in (T_READING, T_CMD_READ):
        client.subscribe(topic, qos=1)
    log.info("Conectado ao broker MQTT — inscrito em %s, %s",
             T_READING, T_CMD_READ)


def _on_message(client, userdata, msg):
    global _cycle_active

    try:
        payload = json.loads(msg.payload.decode("utf-8") or "{}")
    except Exception:
        return

    topic = msg.topic

    # ── Registro de resposta no ciclo atual ───────────────────────────────────
    if topic == T_READING:

        if _cycle_active:
            log.debug("Ciclo de leitura já ativo — registrando resposta")
            pass  # ciclo já ativo, só registra resposta
        else:
            log.debug("Ciclo de leitura iniciado — janela de %ds", RESPONSE_WINDOW_S)

            # Timer que fecha a janela após RESPONSE_WINDOW_S segundos
            t = threading.Timer(RESPONSE_WINDOW_S, _close_cycle)
            t.daemon = True
            t.start()
            
            _cycle_active = True
            _responded_this_cycle.clear()
            
        mac = (payload.get("mac") or "").strip().lower()
        if not mac:
            return

        with _lock:
            # Auto-descoberta via leitura
            if mac not in _known_macs:
                _known_macs.add(mac)
                _miss_count.setdefault(mac, 0)
                log.info("Novo MAC adicionado: %s  (monitorados: %s)",
                         mac, sorted(_known_macs))

            if _cycle_active:
                _responded_this_cycle.add(mac)
                log.info("%-20s  leitura recebida com sucesso", mac)

    # # ── Início de novo ciclo de leitura ───────────────────────────────────────
    # elif topic == T_CMD_READ:
    #     with _lock:
    #         if _cycle_active:
    #             # Ciclo anterior ainda aberto: fecha sem esperar
    #             log.debug("Novo mesh/cmd/read antes do janela anterior fechar — "
    #                       "fechando ciclo anterior imediatamente")
    #             _close_cycle()  # já tem o lock → chamada sem reentrada
    #         _cycle_active = True
    #         _responded_this_cycle.clear()


# ── Ponto de entrada ──────────────────────────────────────────────────────────

def main():
    global _mqtt_client, RESPONSE_WINDOW_S, MISS_THRESHOLD

    parser = argparse.ArgumentParser(
        description="Watchdog de leituras da mesh — publica zeros para sensores ausentes"
    )
    parser.add_argument("--broker", default=DEFAULT_BROKER_HOST,
                        help=f"IP/hostname do broker MQTT (padrão: {DEFAULT_BROKER_HOST})")
    parser.add_argument("--port", type=int, default=DEFAULT_BROKER_PORT,
                        help=f"Porta do broker MQTT (padrão: {DEFAULT_BROKER_PORT})")
    parser.add_argument("--macs", nargs="+", metavar="MAC",
                        help="MACs esperados (opcional — o watchdog também descobre "
                             "automaticamente via mesh/status e mesh/reading)")
    parser.add_argument("--window", type=float, default=RESPONSE_WINDOW_S,
                        help=f"Janela de resposta em segundos (padrão: {RESPONSE_WINDOW_S})")
    parser.add_argument("--threshold", type=int, default=MISS_THRESHOLD,
                        help=f"Ciclos consecutivos para disparar leitura sintética "
                             f"(padrão: {MISS_THRESHOLD})")
    args = parser.parse_args()

    # Sobrescreve globais com valores passados na CLI
    RESPONSE_WINDOW_S = args.window
    MISS_THRESHOLD    = args.threshold

    # Carrega MACs pré-configurados (se fornecidos)
    if args.macs:
        with _lock:
            for m in args.macs:
                mac = m.strip().lower()
                _known_macs.add(mac)
                _miss_count[mac] = 0
        log.info("MACs pré-configurados: %s", sorted(_known_macs))

    log.info("Iniciando watchdog → broker=%s:%d  janela=%ds  threshold=%d ciclos",
             args.broker, args.port, RESPONSE_WINDOW_S, MISS_THRESHOLD)

    _mqtt_client = mqtt.Client(
        callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
        client_id="reading_watchdog",
        clean_session=True,
    )
    _mqtt_client.on_connect = _on_connect
    _mqtt_client.on_message = _on_message

    # Reconexão automática com back-off simples
    while True:
        try:
            _mqtt_client.connect(args.broker, args.port, keepalive=60)
            _mqtt_client.loop_forever()
        except (ConnectionRefusedError, OSError) as exc:
            log.error("Não foi possível conectar ao broker: %s — tentando em 10s", exc)
            time.sleep(10)
        except KeyboardInterrupt:
            log.info("Watchdog encerrado pelo usuário.")
            break


if __name__ == "__main__":
    main()
