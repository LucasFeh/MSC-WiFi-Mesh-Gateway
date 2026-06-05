---
mode: agent
description: Implementa ACK por chunk no OTA via ESP-WIFI-MESH (stop-and-wait ARQ) nos dois repos
---

## Contexto do bug

O OTA via ESP-WIFI-MESH falha com **"buraco: esperado offset X, recebeu Y"** porque o root envia chunks sem esperar confirmação individual. A mesh pode descartar ou reordenar pacotes; sem ARQ, o root avança enquanto o node fica para trás e um gap de 1–2 chunks (× 1024 bytes) quebra o `esp_ota_write` sequencial.

Log representativo que motivou esta correção:
```
W mesh_main: [OTA] chunk duplicado @offset 288768 (já em 289792) — ignorando  ← root retransmite às cegas
E mesh_main: [OTA] buraco: esperado offset 403456, recebeu 405504 — abortando  ← 2 chunks perdidos
```

## O que deve ser implementado

### Stop-and-wait ARQ:
1. Root envia OTA_CHUNK N e **aguarda** `OTA_ACK` com `status=OTA_ACK_OK` e `expected_offset = N + chunk_size`.
2. Só então envia o chunk N+1 com `offset = expected_offset`.
3. Se o ACK não chegar em **2 s**, reenvia o mesmo chunk (até **5 tentativas**; na 6ª marca o nó como falho e cancela o OTA para ele).
4. Se `status=OTA_ACK_FAIL`, cancela imediatamente.

---

## Arquivo 1 — `ota_protocol.h` (COMPARTILHADO entre os dois repos — aplicar identicamente nos dois)

**Localização nos dois repos:**
- NODE/Driver: `main/ota_protocol.h`
- ROOT/Gateway: onde quer que o arquivo esteja (mesmo nome)

**Mudança em `ota_ack_t`:** adicionar o campo `expected_offset` para o node informar ao root qual offset ele espera receber a seguir.

Substituir a struct atual:
```c
typedef struct __attribute__((packed)) {
    uint16_t msg_id;                 /* = BIN_MSG_OTA_ACK                        */
    uint8_t  status;                 /* OTA_ACK_OK / OTA_ACK_FAIL                */
} ota_ack_t;
```
Por:
```c
typedef struct __attribute__((packed)) {
    uint16_t msg_id;                 /* = BIN_MSG_OTA_ACK                        */
    uint8_t  status;                 /* OTA_ACK_OK / OTA_ACK_FAIL                */
    uint32_t expected_offset;        /* próximo offset que o node quer receber   */
                                     /* 0 quando status=OTA_ACK_FAIL             */
} ota_ack_t;
```

---

## Arquivo 2 — NODE `main.cpp`

**Localização:** `main/main.cpp`

### 2a. Alterar `ota_node_send_ack` para aceitar `expected_offset`

Substituir a função atual:
```c
static void ota_node_send_ack(uint8_t status)
{
    ota_ack_t ack = { .msg_id = BIN_MSG_OTA_ACK, .status = status };
    mesh_data_t tx = {
        .data  = (uint8_t *)&ack,
        .size  = sizeof(ack),
        .proto = MESH_PROTO_BIN,
        .tos   = MESH_TOS_P2P,
    };
    esp_mesh_send(NULL, &tx, MESH_DATA_P2P, NULL, 0);
    ESP_LOGI(MESH_TAG, "[OTA] ACK enviado ao root (status=%u)", status);
}
```
Por:
```c
static void ota_node_send_ack(uint8_t status, uint32_t expected_offset)
{
    ota_ack_t ack = {
        .msg_id          = BIN_MSG_OTA_ACK,
        .status          = status,
        .expected_offset = expected_offset,
    };
    mesh_data_t tx = {
        .data  = (uint8_t *)&ack,
        .size  = sizeof(ack),
        .proto = MESH_PROTO_BIN,
        .tos   = MESH_TOS_P2P,
    };
    esp_mesh_send(NULL, &tx, MESH_DATA_P2P, NULL, 0);
    ESP_LOGI(MESH_TAG, "[OTA] ACK enviado ao root (status=%u, next_offset=%u)",
             status, (unsigned)expected_offset);
}
```

### 2b. Atualizar todos os call-sites de `ota_node_send_ack`

Procurar todas as chamadas e ajustar a assinatura:

| Chamada antiga | Nova chamada |
|---|---|
| `ota_node_send_ack(OTA_ACK_FAIL)` | `ota_node_send_ack(OTA_ACK_FAIL, 0)` |
| `ota_node_send_ack(OTA_ACK_OK)` (no OTA_END) | `ota_node_send_ack(OTA_ACK_OK, s_ota_written)` |

### 2c. No case `OTA_CHUNK`: enviar ACK após gravar com sucesso **e** ao rejeitar duplicados

**Situação "chunk duplicado"** — atualmente apenas ignora com `return`. O root não sabe que o node já avançou e continuará retransmitindo o chunk antigo. Após o `return` do duplicado, enviar ACK com o offset atual:

```c
if (pkt->offset < s_ota_written) {
    ESP_LOGW(MESH_TAG, "[OTA] chunk duplicado @offset %u (já em %u) — ignorando",
             (unsigned)pkt->offset, (unsigned)s_ota_written);
    ota_node_send_ack(OTA_ACK_OK, s_ota_written);   /* ← NOVO: informa root onde estamos */
    return;
}
```

**Após gravar com sucesso** — adicionar ACK imediatamente depois de `s_ota_written += n`:

```c
s_ota_written += n;
ota_node_progress(s_ota_written, s_ota_total);
ota_node_send_ack(OTA_ACK_OK, s_ota_written);       /* ← NOVO: stop-and-wait ARQ */
```

### 2d. No case `OTA_BEGIN`: enviar ACK de confirmação após `esp_ota_begin` bem-sucedido

Adicionar após `s_ota_written = 0; s_ota_total = pkt->total; s_ota_last_pct = -1;`:
```c
ota_node_send_ack(OTA_ACK_OK, 0);   /* ← NOVO: root pode começar a enviar chunks */
```

---

## Arquivo 3 — ROOT/Gateway (repo separado)

**Localização:** encontrar onde o root envia os pacotes OTA — provavelmente um arquivo tipo `ota_sender.cpp`, `mesh_ota.cpp`, `ota.cpp`, ou similar.

### Lógica atual (o que remover/substituir)

O root provavelmente tem um loop que:
- Envia todos os chunks em sequência sem esperar ACK
- Tem um timer de retransmissão por timeout baseado em tempo decorrido (não em confirmação)

### Nova lógica de envio (stop-and-wait)

Implementar uma função de envio com retransmissão:

```c
#define OTA_ACK_TIMEOUT_MS   2000   /* ms aguardando ACK de cada chunk */
#define OTA_MAX_RETRIES      5      /* tentativas antes de desistir do nó */

/* Envia um pacote OTA e aguarda o ACK correspondente.
 * Retorna true se o ACK chegou com status=OK e expected_offset correto.
 * Retorna false se falhou (timeout ou status=FAIL). */
static bool ota_root_send_and_wait(const mesh_addr_t *dest,
                                   const ota_packet_t *pkt,
                                   size_t             pkt_size,
                                   uint32_t           expected_next_offset)
{
    for (int attempt = 0; attempt < OTA_MAX_RETRIES; attempt++) {
        /* Enviar o chunk */
        mesh_data_t tx = {
            .data  = (uint8_t *)pkt,
            .size  = pkt_size,
            .proto = MESH_PROTO_BIN,
            .tos   = MESH_TOS_P2P,
        };
        esp_err_t err = esp_mesh_send(dest, &tx, MESH_DATA_P2P, NULL, 0);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "[OTA] esp_mesh_send falhou na tentativa %d: %s",
                     attempt + 1, esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* Aguardar ACK na queue (o rx handler deve colocar ota_ack_t nela) */
        ota_ack_t ack;
        if (xQueueReceive(s_ota_ack_queue, &ack, pdMS_TO_TICKS(OTA_ACK_TIMEOUT_MS)) != pdTRUE) {
            ESP_LOGW(TAG, "[OTA] timeout aguardando ACK @offset %u (tentativa %d/%d)",
                     (unsigned)pkt->offset, attempt + 1, OTA_MAX_RETRIES);
            continue;   /* retransmite */
        }

        if (ack.status == OTA_ACK_FAIL) {
            ESP_LOGE(TAG, "[OTA] nó reportou FAIL @offset %u — abortando", (unsigned)pkt->offset);
            return false;
        }

        if (ack.expected_offset != expected_next_offset) {
            /* Node está em offset diferente: reenviar com o offset correto,
             * ou abortar se estiver à frente (inconsistência grave). */
            ESP_LOGW(TAG, "[OTA] ACK com offset inesperado: esperado %u, recebeu %u",
                     (unsigned)expected_next_offset, (unsigned)ack.expected_offset);
            /* Se o node já avançou além do que enviamos: situação inesperada, abortar. */
            if (ack.expected_offset > expected_next_offset) {
                ESP_LOGE(TAG, "[OTA] node adiantado — abortando");
                return false;
            }
            /* Node ainda está atrás: retransmitir */
            continue;
        }

        return true;   /* ACK OK com offset correto */
    }

    ESP_LOGE(TAG, "[OTA] %d tentativas esgotadas @offset %u — nó considerado perdido",
             OTA_MAX_RETRIES, (unsigned)pkt->offset);
    return false;
}
```

### Queue de ACK

O root precisa de uma `QueueHandle_t s_ota_ack_queue` onde o handler de RX da mesh deposita os `ota_ack_t` recebidos:

```c
/* No início do envio OTA, antes de entrar no loop de chunks: */
s_ota_ack_queue = xQueueCreate(4, sizeof(ota_ack_t));

/* No handler de esp_mesh_recv, quando msg_id == BIN_MSG_OTA_ACK: */
if (msg_id == BIN_MSG_OTA_ACK && data.size >= sizeof(ota_ack_t)) {
    ota_ack_t ack;
    memcpy(&ack, data.data, sizeof(ota_ack_t));
    if (s_ota_ack_queue) {
        xQueueSend(s_ota_ack_queue, &ack, 0);   /* não bloqueia o rx handler */
    }
}

/* Ao final do OTA (sucesso ou falha), destruir a queue: */
vQueueDelete(s_ota_ack_queue);
s_ota_ack_queue = NULL;
```

### Loop de chunks (substituir o loop atual)

```c
/* Enviar OTA_BEGIN e aguardar ACK com expected_offset=0 */
ota_packet_t begin_pkt = { .msg_id = BIN_MSG_OTA, .tipo = OTA_BEGIN, .total = fw_size };
/* ... preencher nome do firmware em payload[] conforme protocolo atual ... */
if (!ota_root_send_and_wait(dest, &begin_pkt, OTA_HDR_SIZE + name_len, 0)) {
    goto ota_failed;
}

/* Loop de chunks: stop-and-wait */
uint32_t offset = 0;
while (offset < fw_size) {
    uint32_t chunk = fw_size - offset;
    if (chunk > OTA_CHUNK_MAX) chunk = OTA_CHUNK_MAX;

    ota_packet_t pkt;
    pkt.msg_id     = BIN_MSG_OTA;
    pkt.tipo       = OTA_CHUNK;
    pkt.total      = fw_size;
    pkt.offset     = offset;
    pkt.chunk_size = chunk;
    memcpy(pkt.payload, fw_data + offset, chunk);
    pkt.crc        = esp_rom_crc32_le(0, pkt.payload, chunk);

    uint32_t next = offset + chunk;
    if (!ota_root_send_and_wait(dest, &pkt, OTA_HDR_SIZE + chunk, next)) {
        goto ota_failed;
    }

    offset = next;
    /* Log de progresso (opcional) */
}

/* Enviar OTA_END e aguardar ACK final */
ota_packet_t end_pkt = { .msg_id = BIN_MSG_OTA, .tipo = OTA_END, .total = fw_size,
                         .offset = fw_size, .chunk_size = 0, .crc = 0 };
if (!ota_root_send_and_wait(dest, &end_pkt, OTA_HDR_SIZE, fw_size)) {
    goto ota_failed;
}
/* OTA concluído com sucesso para este nó */
```

---

## Resumo das mudanças

| Arquivo | Repo | O que muda |
|---|---|---|
| `ota_protocol.h` | **ambos** | `ota_ack_t` ganha campo `expected_offset: uint32_t` |
| `main.cpp` | NODE/Driver | `ota_node_send_ack` aceita `expected_offset`; ACK enviado após cada chunk gravado, após BEGIN e em duplicados |
| `ota_sender.cpp` (ou equivalente) | ROOT/Gateway | Substitui loop fire-and-forget por stop-and-wait com queue de ACK, timeout de 2 s e até 5 retransmissões |

**Cuidado ao aplicar:** como `ota_protocol.h` é compartilhado byte a byte entre os dois repos (conforme comentário no topo do arquivo), a struct `ota_ack_t` **deve ficar idêntica nos dois**. Aplique o arquivo copiando literalmente.
