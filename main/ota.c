#include "ota.h"

/* Non-root: apply incoming firmware packet */
static esp_ota_handle_t      s_ota_handle = 0;
static const esp_partition_t *s_ota_part  = NULL;

/* Broadcast a firmware_packet_t to all mesh nodes */
static void ota_broadcast_pkt(firmware_packet_t *pkt)
{
    static const mesh_addr_t bcast = {.addr = {0xff,0xff,0xff,0xff,0xff,0xff}};
    mesh_data_t d = {
        .data  = (uint8_t *)pkt,
        .size  = (uint16_t)sizeof(firmware_packet_t),
        .proto = MESH_PROTO_BIN,
        .tos   = MESH_TOS_P2P,
    };
    esp_mesh_send(&bcast, &d, MESH_DATA_P2P, NULL, 0);
}


void process_firmware_packet(firmware_packet_t *pkt)
{
    esp_err_t err;
    if (pkt->offset == 0) {
        s_ota_part = esp_ota_get_next_update_partition(NULL);
        if (!s_ota_part) { ESP_LOGE(MESH_TAG, "[OTA] sem partição"); return; }
        err = esp_ota_begin(s_ota_part, pkt->total_size, &s_ota_handle);
        if (err != ESP_OK) {
            ESP_LOGE(MESH_TAG, "[OTA] begin: %s", esp_err_to_name(err));
            s_ota_handle = 0; return;
        }
        ESP_LOGI(MESH_TAG, "[OTA] recebendo %s (%"PRIu32" bytes)", pkt->version, pkt->total_size);
    }
    if (!s_ota_handle) return;
    err = esp_ota_write(s_ota_handle, pkt->data, pkt->data_size);
    if (err != ESP_OK) { ESP_LOGE(MESH_TAG, "[OTA] write: %s", esp_err_to_name(err)); return; }
    uint32_t received = pkt->offset + pkt->data_size;
    uint8_t pct = (uint8_t)((uint64_t)received * 100 / pkt->total_size);
    if (pct % 10 == 0) ESP_LOGI(MESH_TAG, "[OTA] %u%%", pct);
    if (received >= pkt->total_size) {
        err = esp_ota_end(s_ota_handle);
        s_ota_handle = 0;
        if (err != ESP_OK) { ESP_LOGE(MESH_TAG, "[OTA] end: %s", esp_err_to_name(err)); return; }
        err = esp_ota_set_boot_partition(s_ota_part);
        if (err != ESP_OK) { ESP_LOGE(MESH_TAG, "[OTA] set_boot: %s", esp_err_to_name(err)); return; }
        ESP_LOGI(MESH_TAG, "[OTA] completo! reiniciando...");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
}


/* Root: download firmware, write to OTA partition, broadcast packets to children */
static void ota_root_task(void *arg)
{
    char *url = (char *)arg;
    ESP_LOGI(MESH_TAG, "[OTA] root: download de %s", url);

    const esp_partition_t *ota_part = esp_ota_get_next_update_partition(NULL);
    if (!ota_part) { ESP_LOGE(MESH_TAG, "[OTA] sem partição OTA"); goto done; }

    esp_http_client_config_t hcfg = {
        .url = url,
        .timeout_ms = 15000,
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t http = esp_http_client_init(&hcfg);
    if (!http) { ESP_LOGE(MESH_TAG, "[OTA] http init falhou"); goto done; }

    if (esp_http_client_open(http, 0) != ESP_OK) {
        ESP_LOGE(MESH_TAG, "[OTA] http open falhou");
        esp_http_client_cleanup(http); goto done;
    }

    int64_t content_len = esp_http_client_fetch_headers(http);
    ESP_LOGI(MESH_TAG, "[OTA] firmware: %lld bytes", content_len);
    if (content_len <= 0) {
        ESP_LOGE(MESH_TAG, "[OTA] Content-Length inválido (%lld), abortando", content_len);
        esp_http_client_close(http); esp_http_client_cleanup(http); goto done;
    }

    esp_ota_handle_t ota_handle;
    if (esp_ota_begin(ota_part, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle) != ESP_OK) {
        ESP_LOGE(MESH_TAG, "[OTA] ota_begin falhou");
        esp_http_client_close(http); esp_http_client_cleanup(http); goto done;
    }

    firmware_packet_t *pkt = malloc(sizeof(firmware_packet_t));
    if (!pkt) {
        esp_ota_abort(ota_handle);
        esp_http_client_close(http); esp_http_client_cleanup(http); goto done;
    }
    pkt->id         = BIN_MSG_FW_PACKET;
    pkt->total_size = (uint32_t)content_len;
    ESP_LOGI(MESH_TAG, "[OTA] total_size para broadcast: %lu bytes", pkt->total_size);
    pkt->offset     = 0;

    int read_len;
    bool error = false;
    uint8_t last_pct = 0xff;
    while ((read_len = esp_http_client_read(http, (char *)pkt->data, sizeof(pkt->data))) > 0) {
        pkt->data_size = (uint32_t)read_len;
        if (esp_ota_write(ota_handle, pkt->data, read_len) != ESP_OK) {
            ESP_LOGE(MESH_TAG, "[OTA] write falhou"); error = true; break;
        }
        ota_broadcast_pkt(pkt);
        uint32_t done_b = pkt->offset + pkt->data_size;
        uint8_t  pct    = (uint8_t)((uint64_t)done_b * 100 / pkt->total_size);
        if (pct % 10 == 0 && pct != last_pct) {
            ESP_LOGI(MESH_TAG, "[OTA] root %u%%", pct);
            last_pct = pct;
        }
        pkt->offset += pkt->data_size;
    }
    free(pkt);
    esp_http_client_close(http);
    esp_http_client_cleanup(http);

    if (!error) {
        if (esp_ota_end(ota_handle) == ESP_OK &&
            esp_ota_set_boot_partition(ota_part) == ESP_OK) {
            ESP_LOGI(MESH_TAG, "[OTA] root completo! reiniciando...");
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
        } else {
            ESP_LOGE(MESH_TAG, "[OTA] finalização falhou");
        }
    } else {
        esp_ota_abort(ota_handle);
    }

done:
    free(url);
    vTaskDelete(NULL);
}

void trigger_ota(const char *url)
{
    char *copy = strdup(url);
    if (!copy) return;
    xTaskCreate(ota_root_task, "ota", 8192, copy, 5, NULL);
}

/* -------------------------------------------------------------------------- */