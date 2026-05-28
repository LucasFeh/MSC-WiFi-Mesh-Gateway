idf_component_register(SRCS "mesh_main.c" "ota.c" "flask_request.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_wifi esp_event nvs_flash esp_netif esp_http_client driver app_update esp_websocket_client)
