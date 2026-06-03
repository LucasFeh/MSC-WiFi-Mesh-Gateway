#include "mesh_main.h"

static esp_websocket_client_handle_t ws_client = NULL;

/* true = Flask está online e aceitando dados; false = envios pausados */
static volatile bool flask_connected = false;

static void root_ws_task(void *arg);

void ip_event_handler(void *arg, esp_event_base_t event_base,
                      int32_t event_id, void *event_data)
{
    if (event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        is_got_ip = true;
        ESP_LOGI(MESH_TAG, "[IP] got IP:" IPSTR, IP2STR(&event->ip_info.ip));
        if (esp_mesh_is_root())
            xTaskCreate(root_ws_task, "ROOTWS", 8192, NULL, 5, NULL);
    } else {
        is_got_ip = false;
        ESP_LOGW(MESH_TAG, "[IP] IP perdido");
    }
}

/* Extrai o valor string de "key":"value" de um JSON simples (payload controlado
   pelo nosso Flask). Retorna true e preenche out em caso de sucesso. */
static bool ws_json_str(const char *json, const char *key, char *out, size_t outlen)
{
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *k = strstr(json, pat);
    if (!k) return false;
    const char *colon = strchr(k + strlen(pat), ':');
    if (!colon) return false;
    const char *q1 = strchr(colon, '"');
    if (!q1) return false;
    const char *q2 = strchr(q1 + 1, '"');
    if (!q2) return false;
    size_t n = (size_t)(q2 - (q1 + 1));
    if (n >= outlen) n = outlen - 1;
    memcpy(out, q1 + 1, n);
    out[n] = '\0';
    return true;
}

/* "aa:bb:cc:dd:ee:ff" -> mac[6]. Retorna true se os 6 octetos foram lidos. */
static bool parse_mac(const char *s, uint8_t mac[6])
{
    unsigned int b[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x",
               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6)
        return false;
    for (int i = 0; i < 6; i++) {
        if (b[i] > 0xFF) return false;
        mac[i] = (uint8_t)b[i];
    }
    return true;
}

static void ws_event_handler(void *arg, esp_event_base_t base,
                              int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *d = (esp_websocket_event_data_t *)event_data;
    if (event_id == WEBSOCKET_EVENT_DATA && d->op_code == 0x01 && d->data_len > 0) {
        /* WS pode não terminar em '\0': copia para buffer local null-terminado. */
        char buf[256];
        int n = d->data_len < (int)sizeof(buf) - 1 ? d->data_len : (int)sizeof(buf) - 1;
        memcpy(buf, d->data_ptr, n);
        buf[n] = '\0';

        if (strstr(buf, "OTAMON")) {
            /* Liga/desliga a telemetria OTA. Único booleano da mensagem é 'on'. */
            bool on = (strstr(buf, "true") != NULL);
            ESP_LOGI(MESH_TAG, "[WS] Monitor OTA %s", on ? "LIGADO" : "DESLIGADO");
            ota_set_monitor(on);
        } else if (strstr(buf, "\"OTA\"")) {
            char file[64], url[160], target[24];
            if (ws_json_str(buf, "file", file, sizeof(file)) &&
                ws_json_str(buf, "url",  url,  sizeof(url))) {
                /* 'target' é opcional: presente -> unicast para esse MAC;
                   ausente -> roteia por nome (Gateway->self, Driver->mesh). */
                uint8_t mac[6];
                const uint8_t *target_mac = NULL;
                if (ws_json_str(buf, "target", target, sizeof(target)) &&
                    parse_mac(target, mac)) {
                    target_mac = mac;
                    ESP_LOGI(MESH_TAG, "[WS] OTA recebido: file=%s url=%s target=%s", file, url, target);
                } else {
                    ESP_LOGI(MESH_TAG, "[WS] OTA recebido: file=%s url=%s", file, url);
                }
                trigger_ota(url, file, target_mac);
            } else {
                ESP_LOGW(MESH_TAG, "[WS] OTA malformado: %s", buf);
            }
        } else if (strstr(buf, "READ")) {
            ESP_LOGI(MESH_TAG, "[WS] READ_REQUEST recebido");
            pending_read_broadcast = true;
        }
    } else if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        flask_connected = true;
        ESP_LOGI(MESH_TAG, "[WS] Flask online – envio de dados ativado");
    } else if (event_id == WEBSOCKET_EVENT_DISCONNECTED) {
        flask_connected = false;
        ESP_LOGW(MESH_TAG, "[WS] Flask offline – envio de dados pausado");
    }
}

static void root_ws_task(void *arg)
{
    esp_websocket_client_config_t cfg = {
        .uri                  = FLASK_WS_URL,
        .reconnect_timeout_ms = 10000,  /* tenta reconectar a cada 10 s até o Flask ligar */
    };
    ws_client = esp_websocket_client_init(&cfg);
    esp_websocket_register_events(ws_client, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);
    esp_websocket_client_start(ws_client);
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    vTaskDelete(NULL);
}

void post_reading_to_flask(const char *mac_str, uint8_t ch1, uint8_t ch2, uint8_t ch3)
{
    if (!is_got_ip || !flask_connected) return;

    char body[128];
    int n = snprintf(body, sizeof(body),
                     "{\"mac\":\"%s\",\"CH1\":%d,\"CH2\":%d,\"CH3\":%d}",
                     mac_str, ch1, ch2, ch3);

    esp_http_client_config_t cfg = {
        .url        = FLASK_READING_URL,
        .method     = HTTP_METHOD_POST,
        .timeout_ms = 3000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return;
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, n);
    esp_http_client_perform(client);
    esp_http_client_cleanup(client);
}

void post_status_to_flask(const char *mac_str, const char *parent_str, uint8_t layer, int8_t rssi, const char *version)
{
    if (!is_got_ip || !flask_connected) return;

    char body[160];
    int n = snprintf(body, sizeof(body),
                     "{\"mac\":\"%s\",\"parent\":\"%s\",\"layer\":%d,\"rssi\":%d,\"version\":\"%s\"}",
                     mac_str, parent_str, layer, rssi, version);

    esp_http_client_config_t cfg = {
        .url        = FLASK_STATUS_URL,
        .method     = HTTP_METHOD_POST,
        .timeout_ms = 3000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return;
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, n);
    esp_http_client_perform(client);
    esp_http_client_cleanup(client);
}

/* Telemetria OTA: posta um JSON já montado em ota.c. O gate liga/desliga do
   monitor é decidido lá; aqui só checamos conectividade, como nos demais posts. */
void post_ota_event(const char *json_body)
{
    if (!is_got_ip || !flask_connected) return;

    esp_http_client_config_t cfg = {
        .url        = FLASK_OTA_PROGRESS_URL,
        .method     = HTTP_METHOD_POST,
        .timeout_ms = 3000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return;
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json_body, strlen(json_body));
    esp_http_client_perform(client);
    esp_http_client_cleanup(client);
}

void notify_offline(const uint8_t mac[6])
{
    if (!is_got_ip || !flask_connected) return;

    char mac_str[18];
    mac_to_str(mac, mac_str);
    char body[64];
    int n = snprintf(body, sizeof(body), "{\"mac\":\"%s\"}", mac_str);

    esp_http_client_config_t cfg = {
        .url        = "http://192.168.15.213:5000/api/offline",
        .method     = HTTP_METHOD_POST,
        .timeout_ms = 3000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return;
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, n);
    esp_http_client_perform(client);
    esp_http_client_cleanup(client);
}

void led_search_task(void *arg)
{
    while (!is_mesh_connected) {
        gpio_set_level(LED_ROOT_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(150));
        gpio_set_level(LED_ROOT_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    vTaskDelete(NULL);
}