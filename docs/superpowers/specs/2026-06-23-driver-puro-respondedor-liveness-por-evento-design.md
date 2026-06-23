# Driver puro respondedor + liveness por evento (sem heartbeat)

**Data:** 2026-06-23
**Repos afetados:**
- `MSC-WiFi-Mesh-Gateway` (root/firmware + servidor Flask)
- `MSC-SENSOR-LEVEL-DRIVER-WIFI-MESH` (firmware do driver)

## Problema

O driver empurra um `BIN_MSG_STATUS` (heartbeat) a cada 5 s, sem ninguém pedir.
Com N drivers isso é chatter constante na mesh. A **leitura de canais** (sensor)
já é request/response e está correta — **não muda**.

O usuário quer o driver como **puro respondedor**: só responde a requisições do
root, sem heartbeat autônomo. O status de conexão deve ser **orientado a evento**
no lado do root, sem polling de liveness.

## Decisões (travadas)

- **Driver = puro respondedor.** Remover o `BIN_MSG_STATUS` autônomo de 5 s.
- **Telemetria pega carona na resposta de leitura.** Ao responder um
  `READ_REQUEST`, o driver também envia o `status_msg_t` (rssi/layer/versão/
  parent), reusando a struct atual. Atualiza no ritmo da leitura (configurável
  pelo gateway via `mesh/cmd/time`).
- **Liveness por evento no root.** Online/offline derivado dos eventos de mesh
  (entrada/saída de nós).
- **Root manda status + membros SEMPRE juntos.** Nunca o status do root sozinho.
  Dispara: a cada 15 s, ao (re)conectar à rede/broker, na 1ª subida da mesh e
  na entrada/saída de nó.
- **Flask: timeout de 2 min (safety net).** Se nada chega em 120 s, trata como
  offline (além da exclusão da membership / `mesh/offline`).
- **Leitura de canais:** intocada.

## Arquitetura nova

```
Driver (puro respondedor)            Root (gateway)                 Flask
─────────────────────────            ──────────────                 ─────
READ_REQUEST  ───────────────────►  (TX broadcast, período config)
  read_sensor()
  READ_RESPONSE ─────────────────►  RX → post_reading_to_flask ───► mesh/reading
  status_msg_t  ─────────────────►  RX → post_status_to_flask  ───► mesh/status (telemetria)

(sem heartbeat autônomo)
                                     MESH_EVENT_CHILD_CONNECTED  ──► publish_members()
                                     MESH_EVENT_CHILD_DISCONNECTED ► notify_offline + publish_members()
                                     MESH_EVENT_ROUTING_TABLE_ADD/REMOVE ► publish_members()
                                     (periódico 30 s) ──────────────► publish_members()  ──► mesh/members (retained)

Flask: online/offline = pertencer à lista mesh/members (evento), não last_seen.
```

## Mudanças por componente

### A. Driver (`MSC-SENSOR-LEVEL-DRIVER/main/main.cpp`)

1. **Remover o heartbeat autônomo:** no `esp_mesh_p2p_tx_main`, apagar o bloco
   que envia `status_msg_t` a cada 5 s (`last_status_tick`).
2. **Telemetria junto da leitura:** no mesmo TX loop, logo após enviar o
   `READ_RESPONSE` (quando `pending_read_response`), montar e enviar o
   `status_msg_t` (layer/rssi/versão/parent, com o ajuste `-1` do parent_mac já
   existente). Gate por `is_mesh_connected`.
3. Nenhuma mudança no `read_response_t`, no `status_msg_t`, nem no
   `read_timer` (leitura autônoma por silêncio do root permanece).

### B. Root (`MSC-WiFi-Mesh-Gateway/main/`)

1. `flask_request.h`: novo `#define TOPIC_MEMBERS "mesh/members"`; declarar
   `void publish_members(void);`.
2. `flask_request.c`: implementar `publish_members()` — lê
   `esp_mesh_get_routing_table()`, monta `{"macs":["aa:bb:..", ...]}` (exclui o
   próprio MAC do root) e publica em `TOPIC_MEMBERS` com **QoS 1 + retained**.
3. `mesh_main.c`: helper `publish_root_and_members()` — manda o status próprio
   do root (`post_status_to_flask`, layer 1) **e** `publish_members()` na mesma
   rodada. Roda só no TX task (stack folgada).
4. `mesh_main.c` (`mesh_event_handler`): sinalizar `pending_members_publish=true`
   em `MESH_EVENT_CHILD_CONNECTED`, `MESH_EVENT_CHILD_DISCONNECTED` (mantendo o
   `notify_offline`), `MESH_EVENT_ROUTING_TABLE_ADD/REMOVE` e
   `MESH_EVENT_PARENT_CONNECTED` (subida/reconexão da mesh). A flag é consumida
   no TX task → `publish_root_and_members()`.
5. `flask_request.c` (`MQTT_EVENT_CONNECTED`): `pending_members_publish=true`
   (publica status+membros ao (re)conectar ao broker).
6. `mesh_main.c` (`esp_mesh_p2p_tx_main`): periódico de **15 s** (ou flag) →
   `publish_root_and_members()`. Não há mais publish do status do root sozinho.

### C. Flask (`server/app.py`)

1. Novo tópico `T_MEMBERS = "mesh/members"`; assinar em `_on_connect`.
2. `_apply_members(payload)`: monta o set de MACs; para cada device conhecido
   (não layer 1) define `online = mac ∈ set`; cria entrada mínima
   (`online=True`) para MACs novos da lista. Emite `state_update`.
3. `_apply_members`: além de setar `online` pela presença, **reseta `last_seen`**
   dos membros listados (a membership de 15 s mantém o relógio do timeout).
4. `_device_online`: layer 1 → `root_online`; senão →
   `root_online and online and _is_fresh(last_seen)` com
   `DEVICE_OFFLINE_TIMEOUT_S = 120` (2 min).
5. `_apply_status`: atualiza telemetria + `last_seen` + `online=True` (status
   junto da leitura implica vivo).
6. `T_OFFLINE`: mantém marcando `online=False` (sinal imediato).
7. `_watch_root` (loop 1 s) cobre a expiração passiva do timeout de 2 min.

## Convenção de MAC (risco principal)

Os MACs em `mesh/members` (routing table), em `from.addr` (status/leitura) e em
`child->mac` (CHILD_CONNECTED/DISCONNECTED) são o **STA MAC** do nó e devem
casar com a chave que o Flask usa hoje. O ajuste `-1` existente no `parent_mac`
é só para exibir o pai (softAP MAC → STA MAC) e **não** se aplica ao MAC próprio
do nó. → Verificar em hardware que os MACs de `mesh/members` casam com os das
leituras/status (se houver offset de +1, normalizar no `publish_members`).

## Comportamento resultante

- Driver não emite mais nada sozinho; só responde a requisições.
- Telemetria (rssi/layer/versão/parent) chega junto de cada leitura.
- Online quase imediato na entrada (evento de mesh) e offline na saída
  (assoc-expire da mesh ~10 s → CHILD_DISCONNECTED/ROUTING_TABLE_REMOVE).
- `mesh/members` retained ⇒ Flask reconcilia o estado certo ao (re)conectar.

## Não-objetivos (YAGNI)

- Não mexer na leitura de canais nem no período de leitura.
- Não criar UI nova.
- Não mudar o `read_response_t` nem o `status_msg_t`.
- Sem heartbeat por-driver de nenhum tipo (nem lento).

## Verificação

- Driver: confirmar que não há mais envio de status a cada 5 s; o status só sai
  junto do `READ_RESPONSE`.
- Root: ao ligar/desligar um driver, confirmar `mesh/members` publicado (e
  retained) e o `mesh/offline` na queda.
- Flask: bolinha online ao entrar e offline ao sair, **sem** depender de
  `last_seen`; telemetria atualiza no ritmo da leitura.
- Casamento de MAC entre `mesh/members` e as leituras/status (hardware).
- Build limpo dos dois firmwares.
