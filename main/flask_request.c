#include "mesh_main.h"
#include "ota.h"
#include "i2c.h"            /* i2c_macs[], i2c_mac_count, i2c_macs_mutex, MAX_MACS */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static esp_mqtt_client_handle_t s_mqtt = NULL;
static volatile bool            s_mqtt_connected = false;
static volatile bool            s_mqtt_started   = false;

/* ---- parsing de JSON simples (payload controlado por nós: Flask/gateway) ---- */
static bool json_str(const char *json, const char *key, char *out, size_t outlen)
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

static bool parse_mac(const char *s, uint8_t mac[6])
{
    unsigned int b[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6)
        return false;
    for (int i = 0; i < 6; i++) {
        if (b[i] > 0xFF) return false;
        mac[i] = (uint8_t)b[i];
    }
    return true;
}

/* ---- publish helpers (telemetria root -> broker) ---- */
void post_reading_to_flask(const char *mac_str, uint8_t ch1, uint8_t ch2, uint8_t ch3, float tensao)
{
    if (!s_mqtt_connected) return;
    char body[128];
    int n = snprintf(body, sizeof(body),
                     "{\"mac\":\"%s\",\"ch1\":%u,\"ch2\":%u,\"ch3\":%u,\"tensao\":%.2f}",
                     mac_str, ch1, ch2, ch3, tensao);
    esp_mqtt_client_publish(s_mqtt, TOPIC_READING, body, n, 0, 0);   /* QoS 0, não retained */
}

void post_status_to_flask(const char *mac_str, const char *parent_str, uint8_t layer, int8_t rssi, const char *version)
{
    if (!s_mqtt_connected) return;
    char body[176];
    int n = snprintf(body, sizeof(body),
                     "{\"mac\":\"%s\",\"parent\":\"%s\",\"layer\":%d,\"rssi\":%d,\"version\":\"%s\"}",
                     mac_str, parent_str, layer, rssi, version);
    esp_mqtt_client_publish(s_mqtt, TOPIC_STATUS, body, n, 1, 0);    /* QoS 1 */
}

void notify_offline(const uint8_t mac[6])
{
    if (!s_mqtt_connected) return;
    char macs[18];
    mac_to_str(mac, macs);
    char body[48];
    int n = snprintf(body, sizeof(body), "{\"mac\":\"%s\"}", macs);
    esp_mqtt_client_publish(s_mqtt, TOPIC_OFFLINE, body, n, 1, 0);
}

void post_ota_event(const char *json_body)
{
    if (!s_mqtt_connected) return;
    esp_mqtt_client_publish(s_mqtt, TOPIC_OTA_PROGRESS, json_body, 0, 1, 0);  /* len=0 -> strlen */
}

/* ---- comando mesh/cmd/maclist: repõe a lista de MACs a partir do JSON ----
   Aceita {"macs":["aa:bb:cc:dd:ee:ff", ...]}; varre tokens entre aspas e tenta
   ler 6 octetos hex em cada um. "macs" e demais chaves não casam parse_mac. */
static void apply_maclist(const char *json)
{
    uint8_t newlist[MAX_MACS][6];
    int cnt = 0;
    for (const char *p = json; *p && cnt < MAX_MACS; p++) {
        if (*p != '"') continue;
        uint8_t mac[6];
        if (parse_mac(p + 1, mac)) {
            memcpy(newlist[cnt++], mac, 6);
        }
    }
    if (i2c_macs_mutex) xSemaphoreTake(i2c_macs_mutex, portMAX_DELAY);
    for (int i = 0; i < cnt; i++) {
        memcpy(i2c_macs[i], newlist[i], 6);
        memset(&i2c_readings[i], 0, sizeof(i2c_readings[0]));   /* leitura zerada até a 1ª resposta */
    }
    i2c_mac_count = cnt;
    if (i2c_macs_mutex) xSemaphoreGive(i2c_macs_mutex);
    ESP_LOGI(MESH_TAG, "[MQTT] maclist: %d MAC(s)", cnt);
}

/* ---- dispatch de comandos recebidos em mesh/cmd/# ---- */
static void on_command(const char *topic, int tlen, const char *data, int dlen)
{
    char t[48];
    int tn = tlen < (int)sizeof(t) - 1 ? tlen : (int)sizeof(t) - 1;
    memcpy(t, topic, tn); t[tn] = '\0';

    char buf[512];
    int bn = dlen < (int)sizeof(buf) - 1 ? dlen : (int)sizeof(buf) - 1;
    memcpy(buf, data, bn); buf[bn] = '\0';

    if (strcmp(t, "mesh/cmd/read") == 0) {
        pending_read_broadcast = true;
        ESP_LOGI(MESH_TAG, "[MQTT] READ");
    } else if (strcmp(t, "mesh/cmd/otamon") == 0) {
        bool on = (strstr(buf, "true") != NULL);
        ota_set_monitor(on);
        ESP_LOGI(MESH_TAG, "[MQTT] OTAMON %s", on ? "ON" : "OFF");
    } else if (strcmp(t, "mesh/cmd/ota") == 0) {
        char file[64], url[160], target[24];
        if (json_str(buf, "file", file, sizeof(file)) && json_str(buf, "url", url, sizeof(url))) {
            uint8_t mac[6]; const uint8_t *tm = NULL;
            if (json_str(buf, "target", target, sizeof(target)) && parse_mac(target, mac)) tm = mac;
            trigger_ota(url, file, tm);
        } else {
            ESP_LOGW(MESH_TAG, "[MQTT] OTA malformado: %s", buf);
        }
    } else if (strcmp(t, "mesh/cmd/reset") == 0) {
        char target[24]; uint8_t mac[6];
        if (json_str(buf, "target", target, sizeof(target)) && parse_mac(target, mac)) {
            memcpy(reboot_unicast_mac, mac, 6);
            pending_reboot_unicast = true;
            ESP_LOGI(MESH_TAG, "[MQTT] RESET -> %s", target);
        }
    } else if (strcmp(t, "mesh/cmd/markvalid") == 0) {
        char target[24]; uint8_t mac[6];
        if (json_str(buf, "target", target, sizeof(target)) && parse_mac(target, mac)) {
            memcpy(mark_valid_mac, mac, 6);
            pending_mark_valid = true;
            ESP_LOGI(MESH_TAG, "[MQTT] MARKVALID -> %s", target);
        }
    } else if (strcmp(t, "mesh/cmd/maclist") == 0) {
        apply_maclist(buf);
    } else if (strcmp(t, "mesh/cmd/time") == 0) {
        /* {"seconds":N} — período de leitura consumido por read_timer (mesh_main.c). */
        const char *k = strstr(buf, "\"seconds\"");
        const char *colon = k ? strchr(k, ':') : NULL;
        if (colon) {
            int s = atoi(colon + 1);
            if (s > 0) { seconds = s; ESP_LOGI(MESH_TAG, "[MQTT] TIME %d s", s); }
        }
    } else {
        ESP_LOGI(MESH_TAG, "[MQTT] cmd ignorado: %s", t);
    }
}

static void mqtt_event_handler(void *args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_mqtt_connected = true;
        esp_mqtt_client_publish(s_mqtt, TOPIC_ROOT_STATE, "{\"online\":true}", 0, 1, 1);  /* retained */
        esp_mqtt_client_subscribe(s_mqtt, TOPIC_CMD_WILDCARD, 1);
        ESP_LOGI(MESH_TAG, "[MQTT] conectado, envio ativado");
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_mqtt_connected = false;
        ESP_LOGW(MESH_TAG, "[MQTT] desconectado, envio pausado");
        break;
    case MQTT_EVENT_DATA:
        on_command(ev->topic, ev->topic_len, ev->data, ev->data_len);
        break;
    default:
        break;
    }
}

void flask_client_init(void)
{
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
        .session.keepalive  = 30,
        .session.last_will  = {
            .topic   = TOPIC_ROOT_STATE,
            .msg     = "{\"online\":false}",
            .msg_len = 0,         /* 0 -> strlen */
            .qos     = 1,
            .retain  = 1,
        },
    };
    s_mqtt = esp_mqtt_client_init(&cfg);
    if (!s_mqtt) { ESP_LOGE(MESH_TAG, "[MQTT] init falhou"); return; }
    esp_mqtt_client_register_event(s_mqtt, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    /* NÃO iniciar aqui: neste ponto do app_main a pilha TCP/IP (lwip) ainda não
       subiu (start_mesh vem depois). Iniciar antes faz o esp_mqtt_task chamar
       getaddrinfo sem mbox válido -> assert "Invalid mbox". O start ocorre no
       ip_event_handler, quando o root pega IP. */
}

/* IP do root: o esp-mqtt já reconecta sozinho; só logamos. (ROOT-only firmware.) */
void ip_event_handler(void *arg, esp_event_base_t event_base,
                      int32_t event_id, void *event_data)
{
    if (event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        is_got_ip = true;
        ESP_LOGI(MESH_TAG, "[IP] got IP:" IPSTR, IP2STR(&event->ip_info.ip));
        /* TCP/IP pronto: agora é seguro iniciar o cliente MQTT (uma única vez). */
        if (s_mqtt && !s_mqtt_started) {
            esp_mqtt_client_start(s_mqtt);
            s_mqtt_started = true;
            ESP_LOGI(MESH_TAG, "[MQTT] client iniciado (pós-IP)");
        }
    } else {
        is_got_ip = false;
        ESP_LOGW(MESH_TAG, "[IP] IP perdido");
    }
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
