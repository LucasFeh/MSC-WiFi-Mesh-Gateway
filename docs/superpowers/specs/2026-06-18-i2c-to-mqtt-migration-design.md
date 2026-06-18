# Migração I2C → MQTT (root ↔ gateway ↔ Flask)

**Data:** 2026-06-18
**Status:** Aprovado (aguardando revisão do spec → plano de implementação)
**Repos afetados:** `MSC-WiFi-Mesh-Gateway` (root), `MSC-WEBPAGE-PGNSS-DI251829` (gateway), `server/` (Flask)

## Contexto

Hoje a comunicação root ↔ gateway é por **I2C** (root = slave `0x08`, gateway = master).
O gateway lê leituras com `Wire.requestFrom(SLAVE_ADDR, 72)` e envia comandos com
`Wire.write(...)`. O transporte é frágil: leituras de 72 bytes excedem o FIFO de
32 bytes do ESP32 e, sob carga de WiFi/mesh, sofrem underrun → leitura curta →
desalinhamento permanente das fronteiras de 72 bytes → o gateway para de receber.

Paralelamente, o root fala com o Flask por **dois** canais: POSTs HTTP (telemetria↑)
e um **WebSocket** cru (comandos↓). O estado do WS ainda governa o gating dos POSTs.

Decisão: **migrar tudo para MQTT**, com broker Mosquitto rodando no Raspberry (ao
lado do Flask). Sem redundância: quem puder usar MQTT, usa.

## Objetivos

- Substituir o transporte root ↔ gateway (leituras e comandos) por MQTT.
- Substituir o WebSocket root ↔ Flask e os POSTs HTTP de telemetria por MQTT.
- Eliminar o gargalo do I2C de leitura e a sincronização manual de estado.
- Manter o comportamento de Flask, root e gateway equivalente ao de hoje.

## Não-objetivos (mantidos como estão)

- **Download do firmware `.bin`**: continua por HTTP (`GET /firmware/latest.bin`).
  MQTT não é adequado para binário grande.
- **Navegador ↔ Flask**: continua por Socket.IO. O Flask faz a ponte MQTT ↔ Socket.IO.
- **OTA pela mesh** (root → nós driver): inalterado; só o *gatilho* do OTA muda de WS para MQTT.

## Exceção explícita: reboot do root por I2C (MANTIDO)

O único trecho de I2C que **permanece** é o comando de **reboot do root disparado
pelo gateway**: gateway escreve `"UPDT_I2C"` → root chama `esp_restart()`
([i2c.c:117-121](../../../main/i2c.c#L117)). É um canal de recuperação out-of-band
que funciona mesmo sem WiFi/MQTT. Como é só RX de ~8 bytes (cabe no FIFO de 32),
não reintroduz o problema de underrun das leituras de 72 bytes.

Tudo o mais do I2C (leituras de 72 bytes, lista de MACs, COMMIT/UNCOMMIT/CLEAR/
REBOOT/CLICKED/TIME/UPDATE) sai.

## Arquitetura

Broker **Mosquitto no Raspberry**, IP fixo, porta 1883, **anônimo** (LAN confiável).
IP configurável por `#define`/config nos três lados.

Três clientes MQTT:

1. **Root** (`esp-mqtt`, já no ESP-IDF): publica telemetria, assina `mesh/cmd/#`.
   Substitui o `esp_websocket_client` e o worker de POST HTTP de `flask_request.c`.
2. **Flask** (`paho-mqtt`): assina telemetria → atualiza `_devices`/`_ota_state` e
   reemite ao navegador por Socket.IO; publica comandos. Substitui o WS server cru
   (`_RootWSDispatch`/`_root_ws_handler`) e os endpoints HTTP de telemetria.
3. **Gateway** (`PubSubClient`, já instalado): assina leituras, publica comandos.
   Substitui todo o I2C exceto o reboot.

## Mapa de tópicos

### Telemetria ↑ (root publica; Flask + gateway assinam)

| Tópico | Payload (JSON) | QoS | Retained | Origem no root |
|---|---|---|---|---|
| `mesh/reading` | `{"mac","ch1","ch2","ch3","tensao"}` | 0 | não | RX `BIN_MSG_READ_RESPONSE` |
| `mesh/status` | `{"mac","parent","layer","rssi","version"}` | 1 | não | status do nó/root |
| `mesh/offline` | `{"mac"}` | 1 | não | nó stale (rTCounter≥3) |
| `mesh/ota/progress` | JSON OTA (start/progress/ack/done) | 1 | não | quando monitor ligado |
| `mesh/root/state` | `{"online":true\|false}` | 1 | **sim** | conexão / LWT |

### Comandos ↓ (Flask ou gateway publicam; root assina `mesh/cmd/#`)

| Tópico | Payload | QoS | Retained | Ação no root |
|---|---|---|---|---|
| `mesh/cmd/read` | `{}` | 1 | não | `pending_read_broadcast = true` |
| `mesh/cmd/maclist` | `{"macs":["aa:bb:..",...]}` | 1 | **sim** | repõe `i2c_macs[]`/`i2c_mac_count` |
| `mesh/cmd/commit` | `{}` | 1 | não | semântica do COMMIT atual |
| `mesh/cmd/uncommit` | `{}` | 1 | não | semântica do UNCOMMIT atual |
| `mesh/cmd/clear` | `{}` | 1 | não | limpa lista de MACs |
| `mesh/cmd/reset` | `{"target":"aa:bb:.."}` | 1 | não | `pending_reboot_unicast` |
| `mesh/cmd/markvalid` | `{"target":"aa:bb:.."}` | 1 | não | `pending_mark_valid` |
| `mesh/cmd/ota` | `{"file","url","target"?}` | 1 | não | `trigger_ota(url,file,target)` |
| `mesh/cmd/otamon` | `{"on":true\|false}` | 1 | **sim** | `ota_set_monitor(on)` |
| `mesh/cmd/time` | `{"seconds":N}` | 1 | não | ajuste de timer |

### Por que retained nesses três

- `mesh/root/state`: presença via LWT (ver abaixo).
- `mesh/cmd/otamon`: o root reconecta e já recebe o último estado do monitor —
  elimina a sincronização manual que o `_root_ws_handler` faz hoje.
- `mesh/cmd/maclist`: **o root recupera a lista de MACs após um reboot sozinho**.
  Hoje, se o root reinicia, perde a lista até o gateway reenviar.

## Comportamento / casos de borda

- **Presença do root (LWT)**: ao conectar, o root publica `mesh/root/state =
  {"online":true}` (retained). A LWT registrada no CONNECT publica
  `{"online":false}` (retained) se o root cair. O Flask passa a usar isso para o
  root; os nós continuam por `last_seen` (não têm MQTT próprio).
- **Gating de telemetria**: o flag `flask_connected` (hoje amarrado ao WS) é
  removido. O root só publica quando o MQTT está conectado; `esp-mqtt` cuida da
  reconexão e do buffer.
- **Reconexão**: `esp-mqtt`, `paho` (loop) e `PubSubClient` reconectam sozinhos.
- **Navegador intacto**: o Flask, ao receber telemetria MQTT, emite os mesmos
  eventos Socket.IO de hoje (`state_update`, `reading_update`, `ota_progress`).

## Mudanças por componente

### Root (`MSC-WiFi-Mesh-Gateway`)

- `flask_request.c` → cliente MQTT (`esp-mqtt`):
  - Publish helpers: reading / status / offline / ota-progress / root-state.
  - Handler de `mesh/cmd/#` mapeando para `pending_*`, `trigger_ota`, `ota_set_monitor`,
    `i2c_macs[]` (maclist), timer.
  - Remove `esp_websocket_client` e o worker/fila de POST HTTP.
- `mesh_main.c`: no RX `BIN_MSG_READ_RESPONSE`, **publicar** a leitura em `mesh/reading`
  (além de manter `i2c_readings[]` para a lógica de staleness). Remove a criação das
  tasks I2C de leitura/resposta.
- `i2c.c`/`i2c.h`: **reduzir a slave a só RX do reboot** (`UPDT_I2C` → `esp_restart()`).
  Remove `i2c_slave_request_task`, o blob de 72 bytes, e os demais comandos.
- Config: `#define` do IP do broker.

### Flask (`server/app.py`)

- Adiciona `paho-mqtt`; assina `mesh/reading`, `mesh/status`, `mesh/offline`,
  `mesh/ota/progress`, `mesh/root/state`. Callbacks reaproveitam a lógica atual de
  `_devices`/`_ota_state` + `socketio.emit(...)`.
- Publica comandos nos handlers existentes:
  - `read_request` (Socket.IO) → `mesh/cmd/read`
  - `set_ota_monitor` → `mesh/cmd/otamon` (retained)
  - `/api/reset` → `mesh/cmd/reset`; `/api/markvalid` → `mesh/cmd/markvalid`
  - `/api/ota/upload` → salva `.bin` e publica `mesh/cmd/ota`
- Remove: `_RootWSDispatch`, `_root_ws_handler`, `simple_websocket`, e os endpoints
  `/api/status`, `/api/reading`, `/api/offline`, `/api/ota/progress`.
- Mantém: `/api/ota/upload`, `/firmware/latest.bin`, `/api/state`, páginas, Socket.IO.
- `requirements.txt`: adiciona `paho-mqtt`, remove `simple_websocket`.

### Gateway (`MSC-WEBPAGE-PGNSS-DI251829`)

- Adiciona cliente `PubSubClient` (lib já presente) conectando ao broker.
- Substitui os `Wire.write(...)` de comando (em `modbus_comm.cpp`, `utils.cpp`,
  `asyncWeb.cpp`) por publish em `mesh/cmd/*` — **exceto** `UPDT_I2C`, que continua
  por I2C (reboot do root).
- Substitui o `slaveRequest()` (`Wire.requestFrom(SLAVE_ADDR, 72)`) por assinatura de
  `mesh/reading` (e `mesh/offline`/`mesh/status` conforme necessário), alimentando o
  `macFilter()`/display com os mesmos dados.
- Mantém `Wire` inicializado (reboot por I2C + eventuais outros dispositivos I2C).

## Removido (sem redundância)

- WebSocket root ↔ Flask (`esp_websocket_client` + `_root_ws` no Flask).
- POSTs HTTP de telemetria (`/api/status`, `/api/reading`, `/api/offline`, `/api/ota/progress`).
- I2C root ↔ gateway: **leituras de 72 bytes e todos os comandos, exceto o reboot** (`UPDT_I2C`).
- Sobra HTTP só para o `.bin`; Socket.IO só para o navegador; I2C só para o reboot.

## Itens a resolver na fase de plano (mapeamento comando-a-comando)

- `modbus_comm.cpp`: confirmar a semântica de `UPDATE`/`UpdateCommand`/`RBOT_I2C` e
  como mapeiam (provável: `UPDATE`/unicast → `mesh/cmd/ota`; `RBOT_I2C` →
  `mesh/cmd/reset`). Decidir o que vira obsoleto.
- Formato exato do `maclist` (lista completa vs add incremental + clear).
- Confirmar se o `Wire` do gateway é usado por outros periféricos antes de mexer no init.

## Testes / rollout

- Subir Mosquitto no Raspberry; validar pub/sub com `mosquitto_pub`/`mosquitto_sub`.
- Validar cada eixo isoladamente: root→`mesh/reading` visível no broker; comandos do
  Flask/gateway chegando ao root; navegador continua atualizando.
- Validar presença (LWT) desligando o root.
- Validar recuperação da lista de MACs (retained) reiniciando o root.
- Manter reboot por I2C testável (gateway → `UPDT_I2C` → root reinicia).
