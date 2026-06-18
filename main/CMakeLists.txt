idf_component_register(SRCS "mesh_main.c" "flask_request.c" "i2c.c" "ext_watchdog.c" "ota.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_wifi esp_event nvs_flash esp_netif esp_http_client driver app_update esp_websocket_client)
