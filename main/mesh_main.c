#include "mesh_main.h"

const char *MESH_TAG = "mesh_main";

esp_netif_t *netif_sta = NULL;
const uint8_t MESH_ID[6] = { 0x66, 0x66, 0x66, 0x66, 0x66, 0x66};

char FW_VERSION[] = { VERSION, '-', 'G', 'a', 't', 'e', 'w', 'a', 'y', '-', 'R', '\0' };

bool is_mesh_connected        = false;
bool is_got_ip                = false;
volatile bool pending_read_broadcast = false;
volatile bool pending_reboot = false;
volatile bool pending_reboot_unicast = false;  /* reboot unicast para MAC específico */
uint8_t reboot_unicast_mac[6] = {0};
volatile bool pending_mark_valid = false;       /* mark-app-valid agendado (self ou unicast) */
uint8_t mark_valid_mac[6] = {0};

static uint8_t rx_buf[RX_SIZE] = { 0, };
static mesh_addr_t mesh_parent_addr;
static int mesh_layer = -1;
static volatile bool pending_read_response  = false;


typedef struct __attribute__((packed)) {
    uint16_t msg_id;
    uint8_t  ch1;
    uint8_t  ch2;
    uint8_t  ch3;
} read_response_t;

read_response_t queued_response = {0};

void mac_to_str(const uint8_t mac[6], char *out)
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

esp_err_t esp_mesh_comm_p2p_start(void)
{
    static bool is_comm_p2p_started = false;
    if (!is_comm_p2p_started) {
        is_comm_p2p_started = true;
        xTaskCreate(esp_mesh_p2p_tx_main, "MPTX", 8192, NULL, 5, NULL);
        xTaskCreate(esp_mesh_p2p_rx_main, "MPRX", 8192, NULL, 5, NULL);
        xTaskCreate(read_timer, "RHT", 8192, NULL, 5, NULL);
    }
    return ESP_OK;
}

void start_mesh(void)
{
    gpio_reset_pin(LED_ROOT_PIN);
    gpio_set_direction(LED_ROOT_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_ROOT_PIN, 0);

    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_ret);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_create_default_wifi_mesh_netifs(&netif_sta, NULL));
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&config));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, &ip_event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_start());

    uint8_t self_mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, self_mac);
    ESP_LOGI("ROOT", "[MESH] ROOT fixo identificado, iniciando como root");

    ESP_ERROR_CHECK(esp_mesh_init());
    ESP_ERROR_CHECK(esp_event_handler_register(MESH_EVENT, ESP_EVENT_ANY_ID, &mesh_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mesh_set_topology(CONFIG_MESH_TOPOLOGY));
    ESP_ERROR_CHECK(esp_mesh_set_max_layer(CONFIG_MESH_MAX_LAYER));
    ESP_ERROR_CHECK(esp_mesh_set_vote_percentage(1));
    ESP_ERROR_CHECK(esp_mesh_set_xon_qsize(128));
#ifdef CONFIG_MESH_ENABLE_PS
    ESP_ERROR_CHECK(esp_mesh_enable_ps());
    ESP_ERROR_CHECK(esp_mesh_set_ap_assoc_expire(60));
    ESP_ERROR_CHECK(esp_mesh_set_announce_interval(600, 3300));
#else
    ESP_ERROR_CHECK(esp_mesh_disable_ps());
    ESP_ERROR_CHECK(esp_mesh_set_ap_assoc_expire(10));
#endif
    mesh_cfg_t cfg = MESH_INIT_CONFIG_DEFAULT();
    memcpy((uint8_t *) &cfg.mesh_id, MESH_ID, 6);
    cfg.channel = CONFIG_MESH_CHANNEL;

    cfg.router.ssid_len = strlen("MSC-DI251818-NSS004");
    memcpy((uint8_t *) &cfg.router.ssid, "MSC-DI251818-NSS004", cfg.router.ssid_len);
    memcpy((uint8_t *) &cfg.router.password, "Di-Eletrons",
            strlen("Di-Eletrons"));

    ESP_ERROR_CHECK(esp_mesh_set_ap_authmode(CONFIG_MESH_AP_AUTHMODE));
    cfg.mesh_ap.max_connection = CONFIG_MESH_AP_CONNECTIONS;
    cfg.mesh_ap.nonmesh_max_connection = CONFIG_MESH_NON_MESH_AP_CONNECTIONS;
    memcpy((uint8_t *) &cfg.mesh_ap.password, CONFIG_MESH_AP_PASSWD,
           strlen(CONFIG_MESH_AP_PASSWD));
    ESP_ERROR_CHECK(esp_mesh_set_config(&cfg));

    esp_mesh_fix_root(true);
    esp_mesh_set_type(MESH_ROOT);


    ESP_ERROR_CHECK(esp_mesh_start());
    esp_mesh_set_group_id((mesh_addr_t *)&MESH_GROUP_ADDR, 1);

#ifdef CONFIG_MESH_ENABLE_PS
    ESP_ERROR_CHECK(esp_mesh_set_active_duty_cycle(CONFIG_MESH_PS_DEV_DUTY, CONFIG_MESH_PS_DEV_DUTY_TYPE));
    ESP_ERROR_CHECK(esp_mesh_set_network_duty_cycle(CONFIG_MESH_PS_NWK_DUTY, CONFIG_MESH_PS_NWK_DUTY_DURATION, CONFIG_MESH_PS_NWK_DUTY_RULE));
#endif
    ESP_LOGI("ROOT", "mesh starts successfully, heap:%" PRId32 ", %s<%d>%s, ps:%d",
             esp_get_minimum_free_heap_size(),
             esp_mesh_is_root_fixed() ? "root fixed" : "root not fixed",
             esp_mesh_get_topology(), esp_mesh_get_topology() ? "(chain)" : "(tree)",
             esp_mesh_is_ps_enabled());
}

void app_main(void)
{
    ext_wdt_init();
    ext_wdt_start();
    esp_log_level_set("*", ESP_LOG_NONE);
    esp_log_level_set("ROOT", ESP_LOG_INFO);
    esp_log_level_set(MESH_TAG, ESP_LOG_INFO);
    esp_log_level_set("I2C_SLAVE", ESP_LOG_INFO);
    i2c_slave_init();
    xTaskCreate(i2c_slave_task, "I2CSLV", 4096, NULL, 5, NULL);          /* RX: comandos do master */
    xTaskCreate(i2c_slave_request_task, "I2CREQ", 4096, NULL, 5, NULL);  /* TX: resposta contínua ao master */
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

                    /* Atualiza o store consumido pelo onRequest I2C (slave -> master):
                       casa o remetente com a lista i2c_macs[] e grava a leitura fresca,
                       zerando o contador de staleness. */
                        for (int i = 0; i < i2c_mac_count; i++) {
                            if (memcmp(from.addr, i2c_macs[i], 6) == 0) {
                                i2c_readings[i].ch1       = resp->ch1;
                                i2c_readings[i].ch2       = resp->ch2;
                                i2c_readings[i].ch3       = resp->ch3;
                                i2c_readings[i].tensao    = 0;   /* sem tensão na mesh ainda */
                                i2c_readings[i].rTCounter = 0;   /* resposta recebida: fresca */
                                break;
                            }
                        }
                    break;
                case BIN_MSG_STATUS:
                    
                    status_msg_t *s = (status_msg_t *)data.data;
                    char parent_str[18];
                    mac_to_str(from.addr, from_str);
                    mac_to_str(s->parent_mac, parent_str);
                    post_status_to_flask(from_str, parent_str, s->layer, s->rssi, s->version);

                    break;

                case BIN_MSG_OTA_ACK: {
                    /* Confirmação (stop-and-wait) de um nó: status + expected_offset. */
                    ota_ack_t *ack = (ota_ack_t *)data.data;
                    uint32_t eo = (data.size >= sizeof(ota_ack_t)) ? ack->expected_offset : 0;
                    ota_root_register_ack(from.addr, ack->status, eo);
                    break;
                }
            }
        }
    }
    vTaskDelete(NULL);
}

void esp_mesh_p2p_tx_main(void *arg)
{
    static TickType_t last_status_tick = 0;

    while (1) {
        /* Copia a lista com mutex para não bloquear a task I2C durante os envios */
        uint8_t local_macs[MAX_MACS][6];
        int local_count = 0;

        if (pending_read_broadcast) {
            pending_read_broadcast = false;
            uint16_t msg_id = BIN_MSG_READ_REQUEST;
            mesh_data_t tx = {
                .data  = (uint8_t *)&msg_id,
                .size  = sizeof(uint16_t),
                .proto = MESH_PROTO_BIN,
                .tos   = MESH_TOS_P2P,
            };

            if (xSemaphoreTake(i2c_macs_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                local_count = i2c_mac_count;
                memcpy(local_macs, i2c_macs, local_count * 6);
                /* Cada ciclo de broadcast conta como uma "tentativa". O contador é
                   zerado quando a resposta chega (ver RX); ao atingir 3 o onRequest
                   passa a reportar leituras zeradas (sensor offline). */
                for (int i = 0; i < local_count; i++) {
                    if (i2c_readings[i].rTCounter < 3) i2c_readings[i].rTCounter++;
                }
                xSemaphoreGive(i2c_macs_mutex);
            }

            for (int i = 0; i < local_count; i++) {
                mesh_addr_t dest;
                memcpy(dest.addr, local_macs[i], 6);
                esp_mesh_send(&dest, &tx, MESH_DATA_P2P, NULL, 0);
                ESP_LOGI(MESH_TAG, "[TX] READ_REQUEST -> "MACSTR, MAC2STR(dest.addr));
            }
        }

        if(pending_reboot){
            pending_reboot = false;
            uint16_t msg_id = BIN_MSG_REBOOT;
            mesh_data_t tx = {
                .data  = (uint8_t *)&msg_id,
                .size  = sizeof(uint16_t),
                .proto = MESH_PROTO_BIN,
                .tos   = MESH_TOS_P2P,
            };

            for (int i = 0; i < local_count; i++) {
                mesh_addr_t dest;
                memcpy(dest.addr, local_macs[i], 6);
                esp_mesh_send(&dest, &tx, MESH_DATA_P2P, NULL, 0);
                ESP_LOGI(MESH_TAG, "[TX] REBOOT -> "MACSTR, MAC2STR(dest.addr));
            }
        }

        if (pending_reboot_unicast) {
            pending_reboot_unicast = false;
            uint16_t msg_id = BIN_MSG_REBOOT;
            mesh_data_t tx = {
                .data  = (uint8_t *)&msg_id,
                .size  = sizeof(uint16_t),
                .proto = MESH_PROTO_BIN,
                .tos   = MESH_TOS_P2P,
            };
            mesh_addr_t dest;
            memcpy(dest.addr, reboot_unicast_mac, 6);
            esp_mesh_send(&dest, &tx, MESH_DATA_P2P, NULL, 0);
            ESP_LOGI(MESH_TAG, "[TX] REBOOT unicast -> "MACSTR, MAC2STR(dest.addr));
        }

        if (pending_mark_valid) {
            pending_mark_valid = false;
            uint8_t self_mac[6];
            esp_wifi_get_mac(WIFI_IF_STA, self_mac);
            if (memcmp(mark_valid_mac, self_mac, 6) == 0) {
                /* Alvo é o próprio ROOT: confirma a imagem localmente (não roteia
                   pela mesh, pois esp_mesh_send para o próprio MAC não teria efeito). */
                esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
                char *r_suffix = strstr(FW_VERSION, "-R");
                if (r_suffix) *r_suffix = '\0';
                ESP_LOGI(MESH_TAG, "[MARK_VALID] self -> %s", esp_err_to_name(e));
            } else {
                uint16_t msg_id = BIN_MSG_MARK_VALID;
                mesh_data_t tx = {
                    .data  = (uint8_t *)&msg_id,
                    .size  = sizeof(uint16_t),
                    .proto = MESH_PROTO_BIN,
                    .tos   = MESH_TOS_P2P,
                };
                mesh_addr_t dest;
                memcpy(dest.addr, mark_valid_mac, 6);
                esp_mesh_send(&dest, &tx, MESH_DATA_P2P, NULL, 0);
                ESP_LOGI(MESH_TAG, "[TX] MARK_VALID unicast -> "MACSTR, MAC2STR(dest.addr));
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

void mesh_event_handler(void *arg, esp_event_base_t event_base,
                        int32_t event_id, void *event_data)
{
    mesh_addr_t id = {0,};
    static uint16_t last_layer = 0;

    switch (event_id) {
    case MESH_EVENT_STARTED:
        esp_mesh_get_id(&id);
        ESP_LOGI(MESH_TAG, "[MESH] started, ID:"MACSTR"", MAC2STR(id.addr));
        is_mesh_connected = false;
        mesh_layer = esp_mesh_get_layer();
        break;
    case MESH_EVENT_STOPPED:
        ESP_LOGI(MESH_TAG, "[MESH] stopped");
        is_mesh_connected = false;
        mesh_layer = esp_mesh_get_layer();
        break;
    case MESH_EVENT_CHILD_CONNECTED: {
        mesh_event_child_connected_t *child = (mesh_event_child_connected_t *)event_data;
        ESP_LOGI(MESH_TAG, "[MESH] child connected: "MACSTR"", MAC2STR(child->mac));
        break;
    }
    case MESH_EVENT_CHILD_DISCONNECTED: {
        mesh_event_child_disconnected_t *child = (mesh_event_child_disconnected_t *)event_data;
        ESP_LOGI(MESH_TAG, "[MESH] child disconnected: "MACSTR"", MAC2STR(child->mac));
        if (is_got_ip)
            notify_offline(child->mac);
        break;
    }
    case MESH_EVENT_PARENT_CONNECTED: {
        mesh_event_connected_t *connected = (mesh_event_connected_t *)event_data;
        esp_mesh_get_id(&id);
        mesh_layer = connected->self_layer;
        memcpy(&mesh_parent_addr.addr, connected->connected.bssid, 6);
        ESP_LOGI(MESH_TAG, "[MESH] parent connected, layer:%d->%d, parent:"MACSTR"%s",
                 last_layer, mesh_layer, MAC2STR(mesh_parent_addr.addr),
                 esp_mesh_is_root() ? " <ROOT>" : "");
        last_layer = mesh_layer;
        is_mesh_connected = true;
        gpio_set_level(LED_ROOT_PIN, 1);
        esp_netif_dhcpc_stop(netif_sta);
        esp_netif_dhcpc_start(netif_sta);
        esp_mesh_comm_p2p_start();
        break;
    }
    case MESH_EVENT_PARENT_DISCONNECTED: {
        mesh_event_disconnected_t *disconnected = (mesh_event_disconnected_t *)event_data;
        ESP_LOGI(MESH_TAG, "[MESH] parent disconnected, reason:%d", disconnected->reason);
        is_mesh_connected = false;
        mesh_layer = esp_mesh_get_layer();
        xTaskCreate(led_search_task, "LEDSRCH", 2048, NULL, 3, NULL);
        break;
    }
    case MESH_EVENT_LAYER_CHANGE: {
        mesh_event_layer_change_t *layer_change = (mesh_event_layer_change_t *)event_data;
        mesh_layer = layer_change->new_layer;
        ESP_LOGI(MESH_TAG, "[MESH] layer change: %d->%d%s",
                 last_layer, mesh_layer, esp_mesh_is_root() ? " <ROOT>" : "");
        last_layer = mesh_layer;
        break;
    }
    case MESH_EVENT_ROOT_ADDRESS: {
        mesh_event_root_address_t *root_addr = (mesh_event_root_address_t *)event_data;
        ESP_LOGI(MESH_TAG, "[MESH] root address: "MACSTR"", MAC2STR(root_addr->addr));
        break;
    }
    default:
        break;
    }
}


void read_timer(void *arg)
{
    while (1) {
        pending_read_broadcast = true;
        vTaskDelay(pdMS_TO_TICKS(1000)*seconds);
    }
}