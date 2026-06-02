#include "ota.h"
#include <string.h>
#include <stdlib.h>
#include <stddef.h>          /* offsetof */
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"        /* esp_wifi_get_mac, WIFI_IF_STA */
#include "esp_mac.h"         /* MACSTR / MAC2STR              */
#include "esp_rom_crc.h"     /* esp_rom_crc32_le              */

/* OTA_HDR_SIZE vem de ota_protocol.h (compartilhado com o Driver). */
/* -------------------------------------------------------------------------- */
/* Barra de progresso reutilizável nos dois fluxos.
 *   [OTA] self [████████░░░░░░░░░░░░] 40% (204800 / 512000 bytes)
 * 20 blocos: U+2588 (cheio, "\xE2\x96\x88") e U+2591 (vazio, "\xE2\x96\x91"). */
static void ota_print_progress(const char *tag, size_t written, size_t total)
{
    if (total == 0) return;
    int pct = (int)((uint64_t)written * 100 / (uint64_t)total);
    int filled = pct / 5;                 /* 20 blocos -> 1 bloco = 5% */
    if (filled > 20) filled = 20;

    char bar[20 * 3 + 1];                  /* cada bloco UTF-8 ocupa 3 bytes */
    int p = 0;
    for (int i = 0; i < 20; i++) {
        const char *blk = (i < filled) ? "\xE2\x96\x88" : "\xE2\x96\x91";
        bar[p++] = blk[0]; bar[p++] = blk[1]; bar[p++] = blk[2];
    }
    bar[p] = '\0';

    ESP_LOGI(MESH_TAG, "[OTA] %s [%s] %d%% (%u / %u bytes)",
             tag, bar, pct, (unsigned)written, (unsigned)total);
}

/* -------------------------------------------------------------------------- */
/* Estado da distribuição via mesh (Fluxo B) + coleta de ACK dos nós.         */
typedef struct {
    uint8_t mac[6];
    bool    acked;
    uint8_t status;          /* OTA_ACK_OK / OTA_ACK_FAIL */
} ota_target_t;

static ota_target_t      s_targets[CONFIG_MESH_ROUTE_TABLE_SIZE];
static int               s_target_count = 0;
static volatile bool     s_dist_active  = false;
static SemaphoreHandle_t s_dist_mutex   = NULL;

/* Chamado pelo RX da mesh (mesh_main.c) ao receber um OTA_ACK de um nó. */
void ota_root_register_ack(const uint8_t from_mac[6], uint8_t status)
{
    if (!s_dist_mutex || !s_dist_active) return;
    xSemaphoreTake(s_dist_mutex, portMAX_DELAY);
    for (int i = 0; i < s_target_count; i++) {
        if (memcmp(s_targets[i].mac, from_mac, 6) == 0) {
            s_targets[i].acked  = true;
            s_targets[i].status = status;
            break;
        }
    }
    xSemaphoreGive(s_dist_mutex);
    ESP_LOGI(MESH_TAG, "[OTA] ACK de "MACSTR" status=%u", MAC2STR(from_mac), status);
}

/* Envia 'size' bytes de 'pkt' para cada nó alvo (envio direcionado por MAC,
 * MESH_TOS_P2P). esp_mesh_send: docs ESP-WIFI-MESH (ver ota_protocol.h). */
static void ota_send_to_all(const ota_packet_t *pkt, size_t size)
{
    mesh_data_t d = {
        .data  = (uint8_t *)pkt,
        .size  = (uint16_t)size,
        .proto = MESH_PROTO_BIN,
        .tos   = MESH_TOS_P2P,
    };
    for (int i = 0; i < s_target_count; i++) {
        mesh_addr_t to;
        memcpy(to.addr, s_targets[i].mac, 6);
        esp_err_t e = esp_mesh_send(&to, &d, MESH_DATA_P2P, NULL, 0);
        if (e != ESP_OK)
            ESP_LOGW(MESH_TAG, "[OTA] send -> "MACSTR" err 0x%x", MAC2STR(to.addr), e);
    }
}

/* ========================================================================== */
/* Fluxo A: nome contém "Gateway" -> ROOT atualiza a si mesmo.                */
static void ota_self_update_task(void *arg)
{
    char *url = (char *)arg;
    ESP_LOGI(MESH_TAG, "[OTA] self: download de %s", url);

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) { ESP_LOGE(MESH_TAG, "[OTA] sem partição OTA"); goto done; }

    esp_http_client_config_t hcfg = { .url = url, .timeout_ms = 15000, .keep_alive_enable = true };
    esp_http_client_handle_t http = esp_http_client_init(&hcfg);
    if (!http) { ESP_LOGE(MESH_TAG, "[OTA] http init falhou"); goto done; }
    if (esp_http_client_open(http, 0) != ESP_OK) {
        ESP_LOGE(MESH_TAG, "[OTA] http open falhou");
        esp_http_client_cleanup(http); goto done;
    }
    int64_t total = esp_http_client_fetch_headers(http);
    if (total <= 0) {
        ESP_LOGE(MESH_TAG, "[OTA] Content-Length inválido (%lld)", total);
        esp_http_client_close(http); esp_http_client_cleanup(http); goto done;
    }
    ESP_LOGI(MESH_TAG, "[OTA] firmware: %lld bytes", total);

    esp_ota_handle_t h;
    if (esp_ota_begin(part, (size_t)total, &h) != ESP_OK) {
        ESP_LOGE(MESH_TAG, "[OTA] ota_begin falhou");
        esp_http_client_close(http); esp_http_client_cleanup(http); goto done;
    }

    uint8_t *buf = malloc(OTA_CHUNK_MAX);
    if (!buf) { esp_ota_abort(h); esp_http_client_close(http); esp_http_client_cleanup(http); goto done; }

    size_t written = 0; int last_pct = -1; bool err = false; int r;
    while ((r = esp_http_client_read(http, (char *)buf, OTA_CHUNK_MAX)) > 0) {
        if (esp_ota_write(h, buf, r) != ESP_OK) { ESP_LOGE(MESH_TAG, "[OTA] write falhou"); err = true; break; }
        written += r;
        int pct = (int)((uint64_t)written * 100 / (uint64_t)total);
        if (pct != last_pct) { ota_print_progress("self", written, total); last_pct = pct; }
    }
    free(buf);
    esp_http_client_close(http);
    esp_http_client_cleanup(http);

    if (err || written == 0) { esp_ota_abort(h); ESP_LOGE(MESH_TAG, "[OTA] self abortado"); goto done; }
    if (esp_ota_end(h) != ESP_OK)               { ESP_LOGE(MESH_TAG, "[OTA] ota_end falhou");  goto done; }
    if (esp_ota_set_boot_partition(part) != ESP_OK) { ESP_LOGE(MESH_TAG, "[OTA] set_boot falhou"); goto done; }

    ESP_LOGI(MESH_TAG, "[OTA] self completo! reiniciando...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

done:
    free(url);
    vTaskDelete(NULL);
}

/* ========================================================================== */
/* Fluxo B: nome contém "Driver" -> ROOT repassa via mesh, em fluxo (stream), */
/* sem aplicar OTA em si mesmo e sem reiniciar até confirmar entrega (ACK).   */
static void ota_mesh_distribute_task(void *arg)
{
    char *url = (char *)arg;
    ESP_LOGI(MESH_TAG, "[OTA] mesh: download de %s", url);

    if (s_dist_active) { ESP_LOGW(MESH_TAG, "[OTA] distribuição já em andamento, abortando"); goto done; }

    /* 1) Lista de nós a partir da routing table, excluindo o próprio root. */
    mesh_addr_t route[CONFIG_MESH_ROUTE_TABLE_SIZE];
    int route_size = 0;
    if (esp_mesh_get_routing_table(route, sizeof(route), &route_size) != ESP_OK) {
        ESP_LOGE(MESH_TAG, "[OTA] esp_mesh_get_routing_table falhou"); goto done;
    }
    uint8_t self_mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, self_mac);

    xSemaphoreTake(s_dist_mutex, portMAX_DELAY);
    s_target_count = 0;
    for (int i = 0; i < route_size && s_target_count < CONFIG_MESH_ROUTE_TABLE_SIZE; i++) {
        if (memcmp(route[i].addr, self_mac, 6) == 0) continue;   /* não envia para si */
        memcpy(s_targets[s_target_count].mac, route[i].addr, 6);
        s_targets[s_target_count].acked  = false;
        s_targets[s_target_count].status = OTA_ACK_FAIL;
        s_target_count++;
    }
    int N = s_target_count;
    s_dist_active = true;
    xSemaphoreGive(s_dist_mutex);

    ESP_LOGI(MESH_TAG, "[OTA] Nos encontrados: %d", N);
    if (N == 0) { ESP_LOGW(MESH_TAG, "[OTA] nenhum nó na mesh, nada a distribuir"); goto done_active; }

    /* 2) Download em fluxo do Flask. */
    esp_http_client_config_t hcfg = { .url = url, .timeout_ms = 15000, .keep_alive_enable = true };
    esp_http_client_handle_t http = esp_http_client_init(&hcfg);
    if (!http) { ESP_LOGE(MESH_TAG, "[OTA] http init falhou"); goto done_active; }
    if (esp_http_client_open(http, 0) != ESP_OK) {
        ESP_LOGE(MESH_TAG, "[OTA] http open falhou"); esp_http_client_cleanup(http); goto done_active;
    }
    int64_t total = esp_http_client_fetch_headers(http);
    if (total <= 0) {
        ESP_LOGE(MESH_TAG, "[OTA] Content-Length inválido (%lld)", total);
        esp_http_client_close(http); esp_http_client_cleanup(http); goto done_active;
    }
    ESP_LOGI(MESH_TAG, "[OTA] firmware: %lld bytes -> %d nó(s)", total, N);

    ota_packet_t *pkt = malloc(sizeof(*pkt));
    if (!pkt) { esp_http_client_close(http); esp_http_client_cleanup(http); goto done_active; }

    /* 2a) BEGIN (só cabeçalho; carrega 'total'). */
    pkt->msg_id = BIN_MSG_OTA; pkt->tipo = OTA_BEGIN;
    pkt->total = (uint32_t)total; pkt->offset = 0; pkt->chunk_size = 0; pkt->crc = 0;
    ota_send_to_all(pkt, OTA_HDR_SIZE);

    /* 2b) CHUNKs em fluxo. */
    size_t sent = 0; uint32_t offset = 0; int last_pct = -1; int r;
    while ((r = esp_http_client_read(http, (char *)pkt->payload, OTA_CHUNK_MAX)) > 0) {
        pkt->msg_id = BIN_MSG_OTA; pkt->tipo = OTA_CHUNK;
        pkt->total = (uint32_t)total; pkt->offset = offset; pkt->chunk_size = (uint32_t)r;
        pkt->crc = esp_rom_crc32_le(0, pkt->payload, r);   /* [SEM FONTE: esp_rom_crc32_le] — CRC32 do ROM do ESP-IDF (esp_rom/include/esp_rom_crc.h) */
        ota_send_to_all(pkt, OTA_HDR_SIZE + (size_t)r);
        offset += (uint32_t)r; sent += (size_t)r;
        int pct = (int)((uint64_t)sent * 100 / (uint64_t)total);
        if (pct != last_pct) { ota_print_progress("mesh", sent, total); last_pct = pct; }
    }

    /* 2c) END (só cabeçalho). */
    pkt->msg_id = BIN_MSG_OTA; pkt->tipo = OTA_END;
    pkt->total = (uint32_t)total; pkt->offset = offset; pkt->chunk_size = 0; pkt->crc = 0;
    ota_send_to_all(pkt, OTA_HDR_SIZE);

    free(pkt);
    esp_http_client_close(http);
    esp_http_client_cleanup(http);

    /* 3) Aguarda ACK de cada nó (timeout 30 s). ROOT NÃO reinicia. */
    ESP_LOGI(MESH_TAG, "[OTA] envio concluído, aguardando ACK de %d nó(s)...", N);
    TickType_t start = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(30000);
    int acked = 0;
    while ((xTaskGetTickCount() - start) < timeout) {
        acked = 0;
        xSemaphoreTake(s_dist_mutex, portMAX_DELAY);
        for (int i = 0; i < s_target_count; i++) if (s_targets[i].acked) acked++;
        xSemaphoreGive(s_dist_mutex);
        if (acked >= N) break;
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    xSemaphoreTake(s_dist_mutex, portMAX_DELAY);
    for (int i = 0; i < s_target_count; i++) {
        if (!s_targets[i].acked)
            ESP_LOGW(MESH_TAG, "[OTA] nó "MACSTR": SEM ACK (timeout)", MAC2STR(s_targets[i].mac));
        else
            ESP_LOGI(MESH_TAG, "[OTA] nó "MACSTR": %s", MAC2STR(s_targets[i].mac),
                     s_targets[i].status == OTA_ACK_OK ? "OK" : "FALHOU");
    }
    xSemaphoreGive(s_dist_mutex);
    ESP_LOGI(MESH_TAG, "[OTA] distribuição concluída: %d/%d confirmado(s)", acked, N);

done_active:
    s_dist_active = false;
done:
    free(url);
    vTaskDelete(NULL);
}

/* ========================================================================== */
/* Ponto de entrada: decide a rota pelo NOME e dispara a task correspondente. */
void trigger_ota(const char *url, const char *name)
{
    if (!url || !name) { ESP_LOGE(MESH_TAG, "[OTA] url/nome nulo"); return; }
    if (!s_dist_mutex) s_dist_mutex = xSemaphoreCreateMutex();

    /* LOG [OTA] com a decisão ANTES de qualquer operação OTA. */
    if (strstr(name, "Gateway")) {
        ESP_LOGI(MESH_TAG, "[OTA] Arquivo recebido: %s", name);
        ESP_LOGI(MESH_TAG, "[OTA] Destino: ROOT (self-update)");
        char *copy = strdup(url);
        if (copy) xTaskCreate(ota_self_update_task, "ota_self", 8192, copy, 5, NULL);
    } else if (strstr(name, "Driver")) {
        ESP_LOGI(MESH_TAG, "[OTA] Arquivo recebido: %s", name);
        ESP_LOGI(MESH_TAG, "[OTA] Destino: nos via Mesh");
        char *copy = strdup(url);
        if (copy) xTaskCreate(ota_mesh_distribute_task, "ota_mesh", 8192, copy, 5, NULL);
    } else {
        ESP_LOGW(MESH_TAG, "[OTA] nome desconhecido: %s (esperado Gateway*/Driver*), ignorando", name);
    }
}