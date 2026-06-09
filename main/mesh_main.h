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
#include "ext_watchdog.h"

#define LED_ROOT_PIN        2
#define RX_SIZE             (1500)

/* BIN_MSG_FW_PACKET removido: o OTA agora usa BIN_MSG_OTA / BIN_MSG_OTA_ACK
   de ota_protocol.h (incluído via ota.h), com valor único nos dois repos. */
#define BIN_MSG_READ_REQUEST    0x0002
#define BIN_MSG_READ_RESPONSE   0x0003
#define BIN_MSG_STATUS          0x0004
#define BIN_MSG_REBOOT          0x0005
#define BIN_MSG_MARK_VALID      0x0006

extern esp_netif_t *netif_sta;


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
extern volatile bool pending_reboot_unicast;  /* root: reboot unicast agendado     */
extern uint8_t reboot_unicast_mac[6];         /* MAC alvo do reboot unicast        */
extern volatile bool pending_mark_valid;      /* root: mark-app-valid agendado     */
extern uint8_t mark_valid_mac[6];             /* MAC alvo do mark-app-valid        */



void mesh_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
void esp_mesh_p2p_rx_main(void *arg);
void esp_mesh_p2p_tx_main(void *arg);
void mac_to_str(const uint8_t mac[6], char *out /* >=18 bytes */);
void ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
void read_timer(void *arg);

void start_mesh(void);
esp_err_t esp_mesh_comm_p2p_start(void);

#endif /* MESH_MAIN_H_ */
