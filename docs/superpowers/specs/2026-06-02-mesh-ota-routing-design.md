# OTA ESP-WIFI-MESH — Roteamento por nome de arquivo — Design

**Data:** 2026-06-02
**Alvo:** ESP32 (ESP-IDF), topologia mesh com root fixo por MAC
**Repos:**
- ROOT/Gateway: `d:\MICROCONTROLLER_DEV\MSC-WiFi-Mesh-Gateway` (`main/ota.{c,h}`, `main/mesh_main.{c,h}`, `main/flask_request.c`, `server/app.py`, `server/static/script/script.js`)
- NODE/Driver: `D:\MICROCONTROLLER_DEV\Nova pasta\MSC-SENSOR-LEVEL-DRIVER-WIFI-MESH` (`main/main.cpp`, `main/main.h`, `partitions.csv`)

## Problema

Subir um `.bin` pela dashboard não atualiza nada: o caminho OTA é **dead code de ponta a ponta**.

- `trigger_ota()` nunca é chamado (`main/ota.c:137`, sem chamador).
- O ROOT, se o caminho rodasse, faria **self-update + broadcast** sempre, sem olhar o nome (`main/ota.c:103,106,124`).
- O Flask **descarta** o nome do arquivo e nunca avisa o ROOT (`server/app.py:167-169,107`); `_ota_pending_url` é setado e nunca consumido.
- O NODE tem `case BIN_MSG_FW_PACKET: break;` vazio (`main.cpp:43-44`) e `BIN_MSG_FW_PACKET` diverge entre repos (`0x0102` no `ota.h` do root vs `0x0001` nos headers mesh dos dois).
- O NODE não tem partições OTA (`partitions.csv` factory-only, 2 MB).

## Objetivo

OTA roteada pelo nome do `.bin`, sobre a mesh, sem acesso IP externo nos nós:
- `Gateway*.bin` → **ROOT atualiza a si mesmo**.
- `Driver*.bin` → **ROOT repassa via mesh** aos nós, sem se auto-atualizar e sem reiniciar até confirmar entrega.

## Decisões travadas (confirmadas pelo usuário)

| Decisão | Escolha |
|---|---|
| Gatilho + nome | **Push WebSocket**: Flask envia `{"cmd":"OTA","file":"<nome>","url":"<url>"}` no WS já existente |
| Caminho Fluxo B | **Stream Flask→Mesh** (sem buffer RAM do firmware inteiro) |
| Confirmação | **ACK final por nó** (`OTA_ACK(status)` node→root antes do reboot) |
| Escopo NODE | Repo Driver separado, implementado com o mesmo protocolo |
| Flash Driver | **4 MB** → trocar `partitions.csv` para 2-OTA |

## Protocolo de fio compartilhado (idêntico nos dois repos)

Header `ota_protocol.h` replicado em ambos os `main/` (fonte única de verdade):

```
#define BIN_MSG_OTA       0x0010   /* pacote de firmware ROOT->NODE */
#define BIN_MSG_OTA_ACK   0x0011   /* confirmação NODE->ROOT        */
#define OTA_CHUNK_MAX     1024     /* <= 1400 com folga p/ o header  */

enum ota_type { OTA_BEGIN = 0, OTA_CHUNK = 1, OTA_END = 2 };

typedef struct __attribute__((packed)) {
    uint16_t msg_id;                 /* = BIN_MSG_OTA                 */
    uint8_t  tipo;                   /* ota_type                      */
    uint32_t total;                  /* tamanho total do firmware     */
    uint32_t offset;                 /* posição deste chunk           */
    uint32_t chunk_size;             /* bytes válidos em payload       */
    uint32_t crc;                    /* CRC32 do payload (validação)  */
    uint8_t  payload[OTA_CHUNK_MAX];
} ota_packet_t;                      /* ~1043 B  < 1400               */

typedef struct __attribute__((packed)) {
    uint16_t msg_id;                 /* = BIN_MSG_OTA_ACK             */
    uint8_t  status;                 /* 1 = OK, 0 = FAIL              */
} ota_ack_t;
```

CRC: `esp_rom_crc32_le()` (ROM CRC do ESP-IDF). **[SEM FONTE: esp_rom_crc32_le]** — fora da tabela de APIs autorizadas do prompt; é função padrão do ROM (`esp_rom/include/esp_rom_crc.h`). Alternativa sem dependência: CRC32 inline.

## ROOT — recepção e roteamento (`flask_request.c`, `ota.c`)

1. `ws_event_handler` (`flask_request.c:25`) passa a reconhecer `"OTA"` além de `"READ"`. Faz parse mínimo de `file` e `url` do JSON e chama `trigger_ota(url, file)`.
2. `trigger_ota(url, name)` ganha o parâmetro `name`; loga `[OTA]` **antes de agir**:
   - contém `"Gateway"` → destino ROOT (Fluxo A);
   - contém `"Driver"` → destino mesh (Fluxo B);
   - senão → `[OTA] nome desconhecido: <name>, ignorando` e retorna.
3. Dispara a task do fluxo escolhido.

### Fluxo A — self-update (`ota_self_update_task`)
Igual ao `ota_root_task` atual **menos o broadcast**: `esp_http_client_*` → `esp_ota_begin`/`write`/`end` → `set_boot` → `restart`. Usa `ota_print_progress()` por chunk.

### Fluxo B — repasse stream (`ota_mesh_distribute_task`)
1. `esp_mesh_get_routing_table(tbl, sizeof(tbl), &n)`; remove o próprio MAC (`esp_wifi_get_mac(WIFI_IF_STA,…)`); `[OTA] Nos encontrados: N`.
2. Abre HTTP ao Flask. Loop de leitura:
   - monta `ota_packet_t` (tipo BEGIN no primeiro, CHUNK nos demais), `crc = esp_rom_crc32_le(0, payload, n)`;
   - para cada MAC: `esp_mesh_send(&mac, &d, MESH_DATA_P2P, NULL, 0)` com `d.tos = MESH_TOS_P2P`;
   - `ota_print_progress(enviado, total)`.
3. Envia pacote `OTA_END`. **Não** faz `esp_ota_*` em si, **não** `esp_restart`.
4. Aguarda `OTA_ACK` de cada nó (timeout, ex. 30 s); loga `OK`/`FAIL`/`timeout` por MAC. Conclui.

### ROOT RX (`mesh_main.c:174`)
Novo `case BIN_MSG_OTA_ACK`: casa o remetente com a lista de MACs alvo e marca o ACK (flag/contador protegido por mutex ou variável da task de distribuição).

### Progresso (`ota_print_progress(size_t written, size_t total)`)
Função única reutilizada nos dois fluxos:
```
[OTA] [████████░░░░░░░░░░░░] 40% (204800 / 512000 bytes)
```
20 blocos, `█`/`░`, atualizada por chunk.

## NODE — recepção e aplicação (Driver `main.cpp`)

1. `case BIN_MSG_OTA` (renomeado de `BIN_MSG_FW_PACKET`) chama `ota_node_on_packet(&pkt)`:
   - `OTA_BEGIN`: `part = esp_ota_get_next_update_partition(NULL)`; `esp_ota_begin(part, total, &h)`; log `[OTA] recebendo … total`.
   - `OTA_CHUNK`: confere `crc` (recalcula `esp_rom_crc32_le`); `esp_ota_write(h, payload, chunk_size)`; `ota_print_progress`.
   - `OTA_END` (ou `offset+chunk_size >= total`): `esp_ota_end` → `set_boot`; envia `ota_ack_t{status}` ao root via `esp_mesh_send(NULL,…, MESH_DATA_P2P,…)`; `vTaskDelay`; `esp_restart`. Em erro: `esp_ota_abort` + `OTA_ACK(FAIL)`.
2. `partitions.csv` do Driver passa a 2-OTA (flash 4 MB):
   ```
   nvs,      data, nvs,     0x9000,   0x6000,
   otadata,  data, ota,     0xf000,   0x2000,
   phy_init, data, phy,     0x11000,  0x1000,
   ota_0,    app,  ota_0,   0x20000,  0x1D0000,
   ota_1,    app,  ota_1,   0x1F0000, 0x1D0000,
   ```
   `app_main` já chama `esp_ota_mark_app_valid_cancel_rollback()` (`main.cpp:15`), compatível.

## Flask (`server/app.py`, `script.js`)

1. `upload_firmware()` (`app.py:161`): preserva `f.filename` (ex. `Driver-1.bin`); guarda `_ota_pending_name` junto de `_ota_pending_url`; ao final, **empurra** `{"cmd":"OTA","file":<name>,"url":<url>}` ao root via WS (mesmo mecanismo de `handle_read_request`, `app.py:101-107`).
2. `script.js:sendOta()` permanece (já manda o arquivo com nome); ajustar texto de status.

## Componentes e isolamento

| Unidade | Faz o quê | Depende de |
|---|---|---|
| `ota_protocol.h` (compartilhado) | define wire format | — |
| ROOT `ota.c` | decide rota; Fluxo A; Fluxo B (stream+ACK); `ota_print_progress` | `esp_http_client`, `esp_ota`, `esp_mesh` |
| ROOT `flask_request.c` | WS: parse `OTA` → `trigger_ota` | `ota.h` |
| ROOT `mesh_main.c` RX | coleta `OTA_ACK` | estado da task B |
| NODE `main.cpp` | recebe pacotes, aplica OTA, manda ACK | `esp_ota`, `esp_mesh` |
| Flask | preserva nome + push WS | websockets |

## Restrições (do prompt) respeitadas

- ❌ sem `esp_https_ota()` no node; ❌ sem enviar `.bin` inteiro num send; ❌ sem `esp_restart` no ROOT antes dos ACKs (Fluxo B); ❌ sem inventar API (CRC sinalizado `[SEM FONTE]`); ❌ sem refatorar fora do escopo.
- ✅ `OTA_CHUNK_MAX` constante ≤1400; ✅ log `[OTA]` antes da ação; ✅ barra de progresso por chunk; ✅ `esp_mesh_get_routing_table` no Fluxo B.

## Fora de escopo (não tocar)

Divergência pré-existente de `read_response_t` (root sem `volts`, Driver com `float volts`); framing I2C; lógica de leitura de sensores; qualquer refactor não-OTA.

## Riscos / limites

- Stream sem retransmissão por chunk: se um nó perde um chunk, `esp_ota_end` falha → `OTA_ACK(FAIL)` (detectável). Aceito para v1.
- Verificação só por inspeção + build nesta sessão (sem flash em hardware).
- Troca de `partitions.csv` do Driver assume flash 4 MB (confirmado).
