#ifndef MESH_MAIN_H_
#define MESH_MAIN_H_

#include <string.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

#include "ota.h"


#define LED_ROOT_PIN        2
#define RX_SIZE             (1500)

#define BIN_MSG_FW_PACKET       0x0001
#define BIN_MSG_READ_REQUEST    0x0002
#define BIN_MSG_READ_RESPONSE   0x0003
#define BIN_MSG_STATUS          0x0004

/* MAC do nó que será sempre o root fixo */
#define ROOT_MAC  { 0x08, 0xa6, 0xf7, 0x0c, 0x68, 0xc4 }

/* MACs que receberão READ_REQUEST via unicast */
static const uint8_t READ_TARGET_MACS[][6] = {
    { 0x38, 0x18, 0x2b, 0xb2, 0x6f, 0xe8 },
    { 0x94, 0x54, 0xc5, 0x2f, 0x14, 0xe0 },
    { 0x00, 0x4b, 0x12, 0x10, 0x50, 0x60 },
    { 0x1c, 0x69, 0x20, 0xa3, 0xe3, 0x10 },
    { 0x94, 0x54, 0xc5, 0x2f, 0x03, 0x9c },
    { 0x94, 0x54, 0xc5, 0x2f, 0x16, 0x48 },
    { 0xec, 0xe3, 0x34, 0x1b, 0xbb, 0x7c },
    { 0x08, 0xa6, 0xf7, 0xbc, 0x56, 0xe0 },
    { 0x14, 0x33, 0x5c, 0x02, 0xe5, 0xa8 },
    { 0x38, 0x18, 0x2b, 0xb2, 0x11, 0x68 },
    { 0xa0, 0xdd, 0x6c, 0x10, 0x21, 0xd8 },
    { 0xcc, 0xdb, 0xa7, 0x31, 0x75, 0xbc },
    { 0xec, 0x64, 0xc9, 0x91, 0x25, 0x70 },
    { 0xec, 0xe3, 0x34, 0x1b, 0x1e, 0xcc },
    { 0xec, 0xe3, 0x34, 0x1b, 0x9b, 0x68 },
    { 0xfc, 0xe8, 0xc0, 0x79, 0x30, 0x04 },
    { 0x08, 0xa6, 0xf7, 0xbc, 0x44, 0x9c },
    { 0x08, 0xa6, 0xf7, 0xbc, 0x7a, 0xfc },
    { 0x14, 0x33, 0x5c, 0x04, 0x4b, 0x14 },
    { 0x94, 0x54, 0xc5, 0x2f, 0x03, 0xb0 },
    { 0x94, 0x54, 0xc5, 0x2f, 0x28, 0xdc },
    { 0xa0, 0xdd, 0x6c, 0x85, 0xfb, 0x60 },
    { 0x08, 0xa6, 0xf7, 0xbc, 0xf3, 0xa8 },
    { 0x14, 0x33, 0x5c, 0x03, 0x74, 0x3c },
    { 0x14, 0x33, 0x5c, 0x03, 0xe7, 0x58 },
    { 0x1c, 0x69, 0x20, 0xa4, 0x21, 0x48 },
    { 0xd0, 0xef, 0x76, 0x33, 0x8c, 0x7c },
    { 0xec, 0x64, 0xc9, 0x91, 0x96, 0x50 },
    { 0x08, 0xa6, 0xf7, 0xbc, 0x42, 0x3c },
    { 0x5c, 0x01, 0x3b, 0x6d, 0x46, 0xa8 },
};
static const int READ_TARGET_COUNT = sizeof(READ_TARGET_MACS) / sizeof(READ_TARGET_MACS[0]);

/* Grupo mesh para broadcast de leitura */
static const mesh_addr_t MESH_GROUP_ADDR = {.addr = {0xAB, 0xCD, 0xEF, 0x12, 0x34, 0x56}};

typedef struct __attribute__((packed)) {
    uint16_t msg_id;
    uint8_t  ch1;
    uint8_t  ch2;
    uint8_t  ch3;
} read_response_t;

typedef struct __attribute__((packed)) {
    uint16_t msg_id;
    uint8_t  layer;
    int8_t   rssi;
    char     version[8];
    uint8_t  parent_mac[6];
} status_msg_t;

/*******************************************************
 *                Variable Definitions
 *******************************************************/

extern bool is_mesh_connected;
extern bool is_got_ip;

/* Flags de sinalização RX → TX */
extern volatile bool pending_read_broadcast;          /* root: precisa fazer broadcast */

/*******************************************************
 *                Function Declarations
 *******************************************************/
void mesh_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
esp_err_t esp_mesh_comm_p2p_start(void);
void esp_mesh_p2p_rx_main(void *arg);
void esp_mesh_p2p_tx_main(void *arg);
void mac_to_str(const uint8_t mac[6], char *out /* >=18 bytes */);
void ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
void start_mesh(void);


void start_mesh(void){
    gpio_reset_pin(LED_ROOT_PIN);
    gpio_set_direction(LED_ROOT_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_ROOT_PIN, 0);

    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_ret);
    /*  tcpip initialization */
    ESP_ERROR_CHECK(esp_netif_init());
    /*  event initialization */
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    /*  create network interfaces for mesh (only station instance saved for further manipulation, soft AP instance ignored */
    ESP_ERROR_CHECK(esp_netif_create_default_wifi_mesh_netifs(&netif_sta, NULL));
    /*  wifi initialization */
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&config));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, &ip_event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* verifica MAC: root fixo ou modo espera com blink */
    bool is_root_node = false;
    
    uint8_t self_mac[6];
    const uint8_t root_mac[6] = ROOT_MAC;
    esp_wifi_get_mac(WIFI_IF_STA, self_mac);
    if (memcmp(self_mac, root_mac, 6) == 0) {
        ESP_LOGI(MESH_TAG, "[MESH] ROOT fixo identificado, iniciando como root");
        gpio_set_level(LED_ROOT_PIN, 1);
        is_root_node = true;
    } else {
        ESP_LOGI(MESH_TAG, "[MESH] nó não-root: aguardando root...");
    }


    /*  mesh initialization */
    ESP_ERROR_CHECK(esp_mesh_init());
    ESP_ERROR_CHECK(esp_event_handler_register(MESH_EVENT, ESP_EVENT_ANY_ID, &mesh_event_handler, NULL));
    /*  set mesh topology */
    ESP_ERROR_CHECK(esp_mesh_set_topology(CONFIG_MESH_TOPOLOGY));
    /*  set mesh max layer according to the topology */
    ESP_ERROR_CHECK(esp_mesh_set_max_layer(CONFIG_MESH_MAX_LAYER));
    ESP_ERROR_CHECK(esp_mesh_set_vote_percentage(1));
    ESP_ERROR_CHECK(esp_mesh_set_xon_qsize(128));
#ifdef CONFIG_MESH_ENABLE_PS
    /* Enable mesh PS function */
    ESP_ERROR_CHECK(esp_mesh_enable_ps());
    /* better to increase the associate expired time, if a small duty cycle is set. */
    ESP_ERROR_CHECK(esp_mesh_set_ap_assoc_expire(60));
    /* better to increase the announce interval to avoid too much management traffic, if a small duty cycle is set. */
    ESP_ERROR_CHECK(esp_mesh_set_announce_interval(600, 3300));
#else
    /* Disable mesh PS function */
    ESP_ERROR_CHECK(esp_mesh_disable_ps());
    ESP_ERROR_CHECK(esp_mesh_set_ap_assoc_expire(10));
#endif
    mesh_cfg_t cfg = MESH_INIT_CONFIG_DEFAULT();
    /* mesh ID */
    memcpy((uint8_t *) &cfg.mesh_id, MESH_ID, 6);
    /* router: apenas o root conecta ao roteador externo */
    cfg.channel = CONFIG_MESH_CHANNEL;
    if (is_root_node) {
        cfg.router.ssid_len = strlen(CONFIG_MESH_ROUTER_SSID);
        memcpy((uint8_t *) &cfg.router.ssid, CONFIG_MESH_ROUTER_SSID, cfg.router.ssid_len);
        memcpy((uint8_t *) &cfg.router.password, CONFIG_MESH_ROUTER_PASSWD,
               strlen(CONFIG_MESH_ROUTER_PASSWD));
    } else {
        /* SSID fictício: stack não crasha, mas o nó nunca encontrará essa rede
           e portanto nunca será promovido a root por conectividade */
        cfg.router.ssid_len = strlen("MESH_NO_ROUTER");
        memcpy((uint8_t *) &cfg.router.ssid, "MESH_NO_ROUTER", cfg.router.ssid_len);
    }
    /* mesh softAP */
    ESP_ERROR_CHECK(esp_mesh_set_ap_authmode(CONFIG_MESH_AP_AUTHMODE));
    cfg.mesh_ap.max_connection = CONFIG_MESH_AP_CONNECTIONS;
    cfg.mesh_ap.nonmesh_max_connection = CONFIG_MESH_NON_MESH_AP_CONNECTIONS;
    memcpy((uint8_t *) &cfg.mesh_ap.password, CONFIG_MESH_AP_PASSWD,
           strlen(CONFIG_MESH_AP_PASSWD));
    ESP_ERROR_CHECK(esp_mesh_set_config(&cfg));

    /* aplica tipo root fixo */
    esp_mesh_fix_root(true);
    if (is_root_node) {
        esp_mesh_set_type(MESH_ROOT);
    } else {
        esp_mesh_set_type(MESH_NODE);
    }

    /* mesh start */
    ESP_ERROR_CHECK(esp_mesh_start());

    /* todos os nós entram no grupo de broadcast de leitura */
    esp_mesh_set_group_id((mesh_addr_t *)&MESH_GROUP_ADDR, 1);

    /* nó não-root: pisca LED enquanto aguarda conexão com a mesh */
    if (!is_root_node) {
        while (!is_mesh_connected) {
            gpio_set_level(LED_ROOT_PIN, 1);
            vTaskDelay(pdMS_TO_TICKS(150));
            gpio_set_level(LED_ROOT_PIN, 0);
            vTaskDelay(pdMS_TO_TICKS(150));
        }
    }


#ifdef CONFIG_MESH_ENABLE_PS
    /* set the device active duty cycle. (default:10, MESH_PS_DEVICE_DUTY_REQUEST) */
    ESP_ERROR_CHECK(esp_mesh_set_active_duty_cycle(CONFIG_MESH_PS_DEV_DUTY, CONFIG_MESH_PS_DEV_DUTY_TYPE));
    /* set the network active duty cycle. (default:10, -1, MESH_PS_NETWORK_DUTY_APPLIED_ENTIRE) */
    ESP_ERROR_CHECK(esp_mesh_set_network_duty_cycle(CONFIG_MESH_PS_NWK_DUTY, CONFIG_MESH_PS_NWK_DUTY_DURATION, CONFIG_MESH_PS_NWK_DUTY_RULE));
#endif
    ESP_LOGI(MESH_TAG, "mesh starts successfully, heap:%" PRId32 ", %s<%d>%s, ps:%d",  esp_get_minimum_free_heap_size(),
             esp_mesh_is_root_fixed() ? "root fixed" : "root not fixed",
             esp_mesh_get_topology(), esp_mesh_get_topology() ? "(chain)":"(tree)", esp_mesh_is_ps_enabled());
}

esp_err_t esp_mesh_comm_p2p_start(void)
{
    static bool is_comm_p2p_started = false;
    if (!is_comm_p2p_started) {
        is_comm_p2p_started = true;
        xTaskCreate(esp_mesh_p2p_tx_main, "MPTX", 8192, NULL, 5, NULL);
        xTaskCreate(esp_mesh_p2p_rx_main, "MPRX", 8192, NULL, 5, NULL);
    }
    return ESP_OK;
}

#endif /* MESH_MAIN_H_ */
