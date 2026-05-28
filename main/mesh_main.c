#include "mesh_main.h"

const char *MESH_TAG = "mesh_main";

bool is_mesh_connected        = false;
bool is_got_ip                = false;
volatile bool pending_read_broadcast = false;

static const uint8_t MESH_ID[6] = { 0x66, 0x66, 0x66, 0x66, 0x66, 0x66};
static uint8_t rx_buf[RX_SIZE] = { 0, };
static mesh_addr_t mesh_parent_addr;
static int mesh_layer = -1;
static esp_netif_t *netif_sta = NULL;
static volatile bool pending_read_response  = false;
static read_response_t queued_response      = {0};


void mac_to_str(const uint8_t mac[6], char *out)
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void app_main(void)
{
    esp_ota_mark_app_valid_cancel_rollback();
    esp_log_level_set("*", ESP_LOG_NONE);
    esp_log_level_set(MESH_TAG, ESP_LOG_INFO);
    start_mesh();
}

void esp_mesh_p2p_rx_main(void *arg)
{
    esp_err_t err;
    mesh_addr_t from;
    mesh_data_t data;
    int flag = 0;
    data.data = rx_buf;
    data.size = RX_SIZE;

    while (1) {
        data.size = RX_SIZE;
        err = esp_mesh_recv(&from, &data, portMAX_DELAY, &flag, NULL, 0);
        if (err != ESP_OK || !data.size) {
            ESP_LOGE(MESH_TAG, "[MESH RX] err:0x%x, size:%d", err, data.size);
            continue;
        }

        if (data.proto == MESH_PROTO_BIN && data.size >= sizeof(uint16_t)) {
            uint16_t msg_id;
            memcpy(&msg_id, data.data, sizeof(uint16_t));
            switch (msg_id) {
                case BIN_MSG_READ_RESPONSE:
                  
                    read_response_t *resp = (read_response_t *)data.data;
                    char from_str[18];
                    mac_to_str(from.addr, from_str);
                    ESP_LOGI(MESH_TAG, "[READ] de %s CH1:%d CH2:%d CH3:%d", from_str, resp->ch1, resp->ch2, resp->ch3);
                    post_reading_to_flask(from_str, resp->ch1, resp->ch2, resp->ch3);
                
                    break;

                case BIN_MSG_STATUS:
                    
                    status_msg_t *s = (status_msg_t *)data.data;
                    char from_str[18];
                    char parent_str[18];
                    mac_to_str(from.addr, from_str);
                    mac_to_str(s->parent_mac, parent_str);
                    post_status_to_flask(from_str, parent_str, s->layer, s->rssi, s->version);
                    
                    break;
            }
        }
    }
    vTaskDelete(NULL);
}

void esp_mesh_p2p_tx_main(void *arg)
{
    static TickType_t last_status_tick = 0;

    while (1) {
        if (pending_read_broadcast) {
            pending_read_broadcast = false;
            uint16_t msg_id = BIN_MSG_READ_REQUEST;
            mesh_data_t tx = {
                .data  = (uint8_t *)&msg_id,
                .size  = sizeof(uint16_t),
                .proto = MESH_PROTO_BIN,
                .tos   = MESH_TOS_P2P,
            };
            for (int i = 0; i < READ_TARGET_COUNT; i++) {
                mesh_addr_t dest;
                memcpy(dest.addr, READ_TARGET_MACS[i], 6);
                esp_mesh_send(&dest, &tx, MESH_DATA_P2P, NULL, 0);
                ESP_LOGI(MESH_TAG, "[TX] READ_REQUEST -> "MACSTR, MAC2STR(dest.addr));
            }
        }


        TickType_t now = xTaskGetTickCount();
        if (now - last_status_tick >= pdMS_TO_TICKS(5000)) {
            last_status_tick = now;
            wifi_ap_record_t ap_info;
            int8_t rssi = 0;
            if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK)
                rssi = ap_info.rssi;

            if (is_got_ip) {
                uint8_t mac[6];
                char mac_str[18];
                esp_wifi_get_mac(WIFI_IF_STA, mac);
                mac_to_str(mac, mac_str);
                post_status_to_flask(mac_str, "wifi_router", 1, rssi, FW_VERSION);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
    vTaskDelete(NULL);
}
