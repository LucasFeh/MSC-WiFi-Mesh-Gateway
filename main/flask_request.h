#ifndef FLASK_REQUEST_H
#define FLASK_REQUEST_H

#include "mqtt_client.h"
#include "esp_mesh.h"
#include "esp_mesh_internal.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_log.h"

/* Broker MQTT no Raspberry (mesma máquina do Flask). IP fixo, sem auth. */
#define MQTT_BROKER_URI        "mqtt://192.168.15.191:1883"

/* Tópicos — ver docs/superpowers/specs/2026-06-18-i2c-to-mqtt-migration-design.md */
#define TOPIC_READING          "mesh/reading"
#define TOPIC_STATUS           "mesh/status"
#define TOPIC_OFFLINE          "mesh/offline"
#define TOPIC_OTA_PROGRESS     "mesh/ota/progress"
#define TOPIC_ROOT_STATE       "mesh/root/state"
#define TOPIC_CMD_WILDCARD     "mesh/cmd/#"

extern const char *MESH_TAG;

extern void led_search_task(void *arg);
extern void notify_offline(const uint8_t mac[6]);
extern void post_reading_to_flask(const char *mac_str, uint8_t ch1, uint8_t ch2, uint8_t ch3, float tensao);
extern void post_status_to_flask(const char *mac_str, const char *parent_str, uint8_t layer, int8_t rssi, const char *version);
extern void post_ota_event(const char *json_body);
extern void ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
extern void flask_client_init(void);

#endif /* FLASK_REQUEST_H */
