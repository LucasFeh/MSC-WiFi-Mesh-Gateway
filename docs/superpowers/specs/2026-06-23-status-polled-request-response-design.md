# Status polled (request/response) + liveness por timeout

**Data:** 2026-06-23
**Supersede:** `2026-06-23-driver-puro-respondedor-liveness-por-evento-design.md`
**Repos afetados:**
- `MSC-WiFi-Mesh-Gateway` (root/firmware + servidor Flask)
- `MSC-SENSOR-LEVEL-DRIVER-WIFI-MESH` (firmware do driver)

## Problema

A "liveness por evento" (publicar `mesh/status` do root + `mesh/members` em todo
evento de mesh) gerou dois bugs em campo com muitos drivers:

1. **Tempestade de `mesh/status`/`mesh/members`.** Cada `CHILD_CONNECTED`,
   `CHILD_DISCONNECTED`, `ROUTING_TABLE_ADD/REMOVE`, `PARENT_CONNECTED` setava
   `pending_members_publish`, consumido no TX loop a cada 50 ms — sem piso de
   tempo. Com a mesh reorganizando (muitos nós), o root publicava status+membros
   a até ~20×/s, lotando o barramento.
2. **Falso offline em massa.** `_apply_members` tratava qualquer snapshot da
   routing table como autoridade; um snapshot parcial/vazio durante reorg
   derrubava todos os filhos de uma vez. E o timeout de 25 s era apertado demais
   contra a cadência única de 15 s — um publish atrasado já expirava todo mundo
   junto.

## Decisões (travadas)

- **Status vira request/response, simétrico à leitura.** O root pede
  (`BIN_MSG_STATUS_REQUEST`), o driver responde (`BIN_MSG_STATUS`). Sem carona na
  leitura, sem publish por evento.
- **Status a cada 30 s; leitura segue a 60 s** (timers independentes). Status é
  leve (rssi/layer/versão/parent) e **não toca no I2C** — não agrava o lockup.
- **Driver continua puro respondedor.** Só responde a requisições; leitura
  autônoma por silêncio do root (10 min) intocada.
- **Liveness = timeout + offline rápido.** Online = recebeu status/leitura nos
  últimos `DEVICE_OFFLINE_TIMEOUT_S = 90` s (3 polls de status). Offline vem de:
  silêncio > 90 s **ou** `mesh/offline` (sinal imediato no `CHILD_DISCONNECTED`
  de filho direto).
- **Apagar a camada de liveness por evento:** `mesh/members`, `publish_members`,
  `_apply_members`, `pending_members_publish`, `publish_root_and_members`, e os
  `pending_members_publish=true` em todos os event handlers.
- **Leitura de canais e período de 60 s:** intocados.

## Arquitetura nova

```
Driver (puro respondedor)            Root (gateway)                 Flask
─────────────────────────            ──────────────                 ─────
READ_REQUEST  ◄──────────────────── (status_timer NÃO; read_timer 60s)
  read_sensor()
  READ_RESPONSE ────────────────►   RX → post_reading_to_flask ───► mesh/reading

STATUS_REQUEST ◄─────────────────── (status_timer 30s, broadcast p/ todos)
  status_msg_t  ────────────────►   RX → post_status_to_flask  ───► mesh/status
                                     (no mesmo tick: publica status do root, L1)

                                     MESH_EVENT_CHILD_DISCONNECTED ► notify_offline ─► mesh/offline

Flask: online = root_online AND online_flag AND last_seen ≤ 90 s.
       online_flag: True em mesh/status|mesh/reading; False em mesh/offline.
```

## Protocolo (mesh binário)

- **Novo:** `BIN_MSG_STATUS_REQUEST = 0x0007` (root → driver). Mesmo valor nos
  dois repos (`main.h` do driver e `mesh_main.h` do root).
- **Resposta:** `BIN_MSG_STATUS = 0x0004` + `status_msg_t` (struct atual,
  reusada). Driver → root.
- Leitura intocada: `BIN_MSG_READ_REQUEST 0x0002` → `BIN_MSG_READ_RESPONSE 0x0003`.

## Mudanças por componente

### A. Driver (`MSC-SENSOR-LEVEL-DRIVER/main/main.cpp` + `main.h`)

1. `main.h`: `#define BIN_MSG_STATUS_REQUEST 0x0007`; nova flag
   `extern volatile bool pending_status_response`.
2. `main.cpp` RX: `case BIN_MSG_STATUS_REQUEST` → reseta `last_root_request_tick`
   (root vivo) e seta `pending_status_response = true`.
3. `main.cpp` TX: bloco novo `if (pending_status_response)` que monta e envia o
   `status_msg_t` (rssi/layer/versão/parent — o código de hoje, **movido** do
   bloco de leitura). Gate por `is_mesh_connected`.
4. `main.cpp` TX: **remover** o bloco de status que pegava carona no
   `READ_RESPONSE`. Read response volta a carregar só a leitura.

### B. Root (`MSC-WiFi-Mesh-Gateway/main/`)

1. `mesh_main.h`: `#define BIN_MSG_STATUS_REQUEST 0x0007`; nova flag
   `extern volatile bool pending_status_broadcast`; `void status_timer(void *arg)`.
   **Remover** `pending_members_publish`.
2. `mesh_main.c`: flag `pending_status_broadcast`; task `status_timer` (30 s)
   espelhando `read_timer`. Criar a task em `esp_mesh_comm_p2p_start`.
3. `mesh_main.c` TX: bloco que, em `pending_status_broadcast`, faz broadcast de
   `BIN_MSG_STATUS_REQUEST` para todos os `i2c_macs` **e** publica o status do
   próprio root (`post_status_to_flask`, layer 1, rssi/versão).
4. `mesh_main.c`: **apagar** `publish_root_and_members`, o periódico de 15 s, e os
   `pending_members_publish=true` em `CHILD_CONNECTED`, `ROUTING_TABLE_ADD/REMOVE`,
   `PARENT_CONNECTED`. **Manter** `notify_offline` no `CHILD_DISCONNECTED`.
5. `flask_request.c`/`.h`: **apagar** `publish_members` e `TOPIC_MEMBERS`.
   `MQTT_EVENT_CONNECTED` seta `pending_status_broadcast = true` (repovoa a UI).
   **Manter** `notify_offline` e `mesh/root/state` (retained + LWT).

### C. Flask (`server/app.py`)

1. **Apagar** `T_MEMBERS`, `_apply_members`, a assinatura e o handler de members.
2. `DEVICE_OFFLINE_TIMEOUT_S = 90`.
3. `_device_online`: layer 1 → `root_online`; senão →
   `root_online AND online_flag AND _is_fresh(last_seen)`. `online_flag` é
   `True` via `_apply_status`, `False` via `T_OFFLINE`.
4. Manter `_apply_status` (online=True, last_seen=now), `T_OFFLINE`, `_watch_root`.

## Hygiene / migração

- Limpar o retained de `mesh/members` no broker:
  `mosquitto_pub -h localhost -t mesh/members -r -n`.
- Conferir se há `mesh/cmd/time` retained com período pequeno (origem investigada
  no debug do storm de leitura): `mosquitto_sub -t 'mesh/cmd/#' -v`; se houver,
  `mosquitto_pub -t mesh/cmd/time -r -n`.

## Comportamento resultante

- `mesh/status` chega ~1×/30 s por driver; `mesh/reading` ~1×/60 s. Sem storm.
- Online ≤ 30 s após o driver voltar a responder; offline por silêncio em ≤ 90 s
  (tolera 2 polls perdidos sem falso-offline), e imediato via `mesh/offline` no
  disconnect de filho direto.
- Um poll de status perdido **não** derruba a malha inteira (margem 90 s/30 s).

## Não-objetivos (YAGNI)

- Não mexer na leitura de canais nem no período de 60 s.
- Não corrigir o rate-limit do READ_REQUEST do driver (comentário 30 s vs código
  5 s) — bug separado, fora de escopo; apenas anotado.
- Sem heartbeat autônomo no driver de nenhum tipo.

## Verificação

- Flask (pytest): status fresco → online; silêncio > 90 s → offline; um poll
  perdido (30–60 s) **não** derruba ninguém; `mesh/offline` → offline imediato.
- Firmware (integração, `mosquitto_sub`): status ~30 s, leitura ~60 s, sem storm;
  offline ao desligar um driver.
- Casamento de MAC entre `mesh/status`/`mesh/reading` e a chave do Flask.
- Build limpo dos dois firmwares.
