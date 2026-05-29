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
#include "i2c.h"

#define LED_ROOT_PIN        2
#define RX_SIZE             (1500)

#define BIN_MSG_FW_PACKET       0x0001
#define BIN_MSG_READ_REQUEST    0x0002
#define BIN_MSG_READ_RESPONSE   0x0003
#define BIN_MSG_STATUS          0x0004

extern esp_netif_t *netif_sta;

/* MAC do nó que será sempre o root fixo */
#define ROOT_MAC  { 0x08, 0xa6, 0xf7, 0x0c, 0x68, 0xc4 }
extern const uint8_t MESH_ID[6];

/* Grupo mesh para broadcast de leitura */
static const mesh_addr_t MESH_GROUP_ADDR = {.addr = {0xAB, 0xCD, 0xEF, 0x12, 0x34, 0x56}};



typedef struct __attribute__((packed)) {
    uint16_t msg_id;
    uint8_t  layer;
    int8_t   rssi;
    char     version[8];
    uint8_t  parent_mac[6];
} status_msg_t;

extern bool is_mesh_connected;
extern bool is_got_ip;

/* Flags de sinalização RX → TX */
extern volatile bool pending_read_broadcast;          /* root: precisa fazer broadcast */


void mesh_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
void esp_mesh_p2p_rx_main(void *arg);
void esp_mesh_p2p_tx_main(void *arg);
void mac_to_str(const uint8_t mac[6], char *out /* >=18 bytes */);
void ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);


void start_mesh(void);
esp_err_t esp_mesh_comm_p2p_start(void);

#endif /* MESH_MAIN_H_ */
