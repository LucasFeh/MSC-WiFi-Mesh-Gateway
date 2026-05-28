#ifndef FLASK_REQUEST_H
#define FLASK_REQUEST_H

#include "esp_websocket_client.h"
#include "esp_mesh.h"
#include "esp_mesh_internal.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_log.h"


#define FLASK_SERVER_URL    "http://192.168.15.213:5000/api/log"
#define FLASK_READING_URL   "http://192.168.15.213:5000/api/reading"
#define FLASK_STATUS_URL    "http://192.168.15.213:5000/api/status"
#define FLASK_WS_URL        "ws://192.168.15.213:5001"
#define FW_VERSION          "7.0.0"


extern const char *MESH_TAG;

extern void led_search_task(void *arg);
extern void notify_offline(const uint8_t mac[6]);
extern void post_reading_to_flask(const char *mac_str, uint8_t ch1, uint8_t ch2, uint8_t ch3);
extern void post_status_to_flask(const char *mac_str, const char *parent_str, uint8_t layer, int8_t rssi, const char *version);
extern void ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

#endif /* FLASK_REQUEST_H */