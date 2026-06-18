# MQTT Migration — ROOT (plan 1 of 3) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the ROOT's Flask WebSocket + HTTP telemetry with an MQTT client (publish telemetry, subscribe to `mesh/cmd/#`), and strip the I2C slave down to the reboot-only command.

**Architecture:** `flask_request.c/.h` is reimplemented over `esp-mqtt` (built into ESP-IDF), keeping the **same public function names** so `mesh_main.c`/`ota.c` change minimally. The blocking-HTTP queue/worker is removed (esp-mqtt publish is non-blocking). `i2c.c` is reduced to a receive-only slave that handles `UPDT_I2C` → `esp_restart()`. Contract is the topic map in `docs/superpowers/specs/2026-06-18-i2c-to-mqtt-migration-design.md`.

**Tech Stack:** ESP-IDF v5.2.7, `esp-mqtt` (`mqtt_client.h`), FreeRTOS.

**Cross-plan note:** Until the Gateway plan ships, the ROOT no longer receives the MAC list over I2C. Integration tests here publish `mesh/cmd/maclist` manually with `mosquitto_pub`.

---

## File Structure

- Modify `main/flask_request.h` — swap WS/HTTP includes+defines for MQTT broker URI + topic constants; extend `post_reading_to_flask` signature with `tensao`.
- Modify `main/flask_request.c` — full reimplementation over esp-mqtt (client init+LWT, connect/subscribe, publish helpers, command dispatch, maclist apply). Removes the queue/worker and the WS client.
- Modify `main/i2c.c` / `main/i2c.h` — reduce slave to reboot-only RX; keep `i2c_macs[]`/`i2c_readings[]`/`i2c_macs_mutex` definitions.
- Modify `main/mesh_main.c` — pass `tensao` to `post_reading_to_flask`; remove creation of `i2c_slave_request_task`.
- Modify `main/CMakeLists.txt` — `REQUIRES`: drop `esp_websocket_client`, add `mqtt`.
- Modify `main/idf_component.yml` — drop the `esp_websocket_client` managed dependency.

---

## Task 1: MQTT config + public header

**Files:**
- Modify: `main/flask_request.h`

- [ ] **Step 1: Replace the header contents**

Replace the entire body of `main/flask_request.h` with:

```c
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
```

- [ ] **Step 2: Commit (compiles only after Task 2; commit header + impl together)**

Defer commit to Task 2 Step 4 (header and implementation must land together to build).

---

## Task 2: Reimplement `flask_request.c` over esp-mqtt

**Files:**
- Modify: `main/flask_request.c`
- Modify: `main/CMakeLists.txt`
- Modify: `main/idf_component.yml`

- [ ] **Step 1: Replace the entire `main/flask_request.c` with the MQTT implementation**

```c
#include "mesh_main.h"
#include "ota.h"
#include "i2c.h"            /* i2c_macs[], i2c_mac_count, i2c_macs_mutex, MAX_MACS */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static esp_mqtt_client_handle_t s_mqtt = NULL;
static volatile bool            s_mqtt_connected = false;

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
    esp_mqtt_client_start(s_mqtt);
}

/* IP do root: o esp-mqtt já reconecta sozinho; só logamos. (ROOT-only firmware.) */
void ip_event_handler(void *arg, esp_event_base_t event_base,
                      int32_t event_id, void *event_data)
{
    if (event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        is_got_ip = true;
        ESP_LOGI(MESH_TAG, "[IP] got IP:" IPSTR, IP2STR(&event->ip_info.ip));
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
```

- [ ] **Step 2: Update `main/CMakeLists.txt` REQUIRES**

Replace line 3 so it drops `esp_websocket_client` and adds `mqtt`:

```cmake
                    REQUIRES esp_wifi esp_event nvs_flash esp_netif esp_http_client driver app_update mqtt)
```

- [ ] **Step 3: Update `main/idf_component.yml`**

Replace the whole file with (no managed deps needed; `mqtt` ships with IDF):

```yaml
dependencies: {}
```

- [ ] **Step 4: Build**

Run (no ESP-IDF terminal needed if you have `idf.py` exported):
```
idf.py build
```
Expected: build succeeds. If it complains about `esp_websocket_client.h` still being referenced, grep: `grep -rn esp_websocket main/` must return nothing.

- [ ] **Step 5: Commit**

```bash
git add main/flask_request.h main/flask_request.c main/CMakeLists.txt main/idf_component.yml
git commit -m "feat(root): MQTT client substitui WebSocket+HTTP do Flask"
```

---

## Task 3: Pass `tensao` on reading + drop the I2C readings task

**Files:**
- Modify: `main/mesh_main.c:176` (call site of `post_reading_to_flask`)
- Modify: `main/mesh_main.c` (app_main: remove `i2c_slave_request_task` creation)

- [ ] **Step 1: Pass tensão to the publisher**

In `main/mesh_main.c`, in the `BIN_MSG_READ_RESPONSE` case, replace:
```c
                    post_reading_to_flask(from_str, resp->ch1, resp->ch2, resp->ch3);
```
with:
```c
                    post_reading_to_flask(from_str, resp->ch1, resp->ch2, resp->ch3, resp->volts / 15.6f);
```

- [ ] **Step 2: Stop creating the I2C TX-readings task in app_main**

In `main/mesh_main.c`, in `app_main`, delete this line (the 72-byte response task is gone):
```c
    xTaskCreate(i2c_slave_request_task, "I2CREQ", 4096, NULL, 5, NULL);  /* TX: resposta contínua ao master */
```
Keep `i2c_slave_init()` and `xTaskCreate(i2c_slave_task, ...)` (reduced to reboot-only in Task 4).

- [ ] **Step 3: Build**

```
idf.py build
```
Expected: succeeds.

- [ ] **Step 4: Commit**

```bash
git add main/mesh_main.c
git commit -m "feat(root): publica tensão na leitura MQTT e remove task de resposta I2C"
```

---

## Task 4: Reduce `i2c.c` to reboot-only RX

**Files:**
- Modify: `main/i2c.c`
- Modify: `main/i2c.h`

- [ ] **Step 1: Trim `main/i2c.h`**

Remove the now-unused TX/blob/command declarations. Keep the data the mesh still uses (`i2c_macs`, `i2c_mac_count`, `i2c_macs_mutex`, `i2c_readings`, `MAX_MACS`, the pin/addr/buf defines), and keep `i2c_slave_init` + `i2c_slave_task`. Delete the declaration of `i2c_slave_request_task` and the `I2C_REQUEST_STRIDE`/`I2C_TX_DEPTH_RECORDS`/`I2C_TX_BUF_SIZE`/`I2C_REQUEST_BLOB_MAX` macros. Replace the prototypes block with:
```c
void i2c_slave_init(void);
void i2c_slave_task(void *arg);          /* RX: master -> slave (apenas reboot UPDT_I2C) */
```

- [ ] **Step 2: Replace the body of `main/i2c.c` command handling with reboot-only**

Delete `i2c_emit_record`, `i2c_build_request_blob`, `i2c_slave_request_task`, `parseMAC`, `i2c_is_mac_token`, `i2c_split_and_dispatch`, and the full `i2c_on_receive` dispatcher. Keep the global definitions (`i2c_macs`, `i2c_mac_count`, `i2c_macs_mutex`, `i2c_readings`, the control-surface flags) and `i2c_slave_init`. Replace `i2c_slave_task` with:

```c
void i2c_slave_task(void *arg)
{
    uint8_t rx_data[I2C_RX_BUF_SIZE];
    char    printable[I2C_RX_BUF_SIZE + 1];

    ESP_LOGI(TAG, "Task I2C (reboot-only) iniciada (timeout %d ms)", I2C_READ_TIMEOUT_MS);

    while (1) {
        int len = i2c_slave_read_buffer(I2C_SLAVE_PORT, rx_data, sizeof(rx_data),
                                        pdMS_TO_TICKS(I2C_READ_TIMEOUT_MS));
        if (len <= 0) continue;

        int n = (len < (int)sizeof(printable) - 1) ? len : (int)sizeof(printable) - 1;
        memcpy(printable, rx_data, n);
        printable[n] = '\0';

        /* Único comando mantido por I2C: reboot do root (recuperação out-of-band). */
        if (strstr(printable, "UPDT_I2C")) {
            ESP_LOGW(TAG, "[I2C] UPDT_I2C -> esp_restart()");
            esp_restart();
        }
    }
}
```

Note: `i2c_driver_install` in `i2c_slave_init` still passes `I2C_TX_BUF_SIZE`. Replace that argument with `0` (no TX needed):
```c
    ESP_ERROR_CHECK(i2c_driver_install(I2C_SLAVE_PORT, conf.mode,
                                       I2C_RX_BUF_SIZE, 0, 0));
```
And remove the `mac_added_sem` / `mac_added_sem` usage if it remains (it was only used by the removed request task). Keep `i2c_macs_mutex` creation in `i2c_slave_init`.

- [ ] **Step 3: Build**

```
idf.py build
```
Expected: succeeds. Grep to confirm the blob path is gone: `grep -n "requestFrom\|I2C_REQUEST_STRIDE\|i2c_slave_request_task" main/i2c.c main/i2c.h` returns nothing.

- [ ] **Step 4: Commit**

```bash
git add main/i2c.c main/i2c.h
git commit -m "feat(root): I2C slave reduzido a reboot-only (UPDT_I2C)"
```

---

## Task 5: Integration test against the broker

**Files:** none (runtime verification)

- [ ] **Step 1: Start Mosquitto on the Raspberry**

On the Pi: `sudo systemctl start mosquitto` (or `mosquitto -v`). Confirm it listens on `192.168.15.191:1883`.

- [ ] **Step 2: Flash the ROOT and observe connect**

```
idf.py -p <PORTA> flash monitor
```
Expected serial: `[MQTT] conectado, envio ativado`.

- [ ] **Step 3: Confirm presence + subscribe with a broker client**

On any machine:
```
mosquitto_sub -h 192.168.15.191 -t 'mesh/#' -v
```
Expected: a retained `mesh/root/state {"online":true}` appears immediately on subscribe.

- [ ] **Step 4: Inject a MAC list and trigger a read**

```
mosquitto_pub -h 192.168.15.191 -t mesh/cmd/maclist -r -m '{"macs":["aa:bb:cc:dd:ee:01"]}'
mosquitto_pub -h 192.168.15.191 -t mesh/cmd/read -m '{}'
```
Expected serial: `[MQTT] maclist: 1 MAC(s)` then `[MQTT] READ`; and, when a node answers, a `mesh/reading {...,"tensao":..}` shows up in `mosquitto_sub`.

- [ ] **Step 5: Confirm LWT**

Kill the ROOT (reset button). Expected in `mosquitto_sub`: `mesh/root/state {"online":false}` (published by the broker on LWT).

- [ ] **Step 6: Confirm reboot-by-I2C still works**

With the gateway still on old firmware (or a bench master) sending `UPDT_I2C` over I2C, the ROOT logs `[I2C] UPDT_I2C -> esp_restart()` and reboots.

- [ ] **Step 7: Commit a short note (optional)**

No code; record results in the PR/commit message of Task 4 or a follow-up.

---

## Self-Review

- **Spec coverage (ROOT rows):** `mesh/reading` (Task 3), `mesh/status`/`mesh/offline`/`mesh/ota/progress` (Task 2 publishers), `mesh/root/state` retained+LWT (Task 2), commands `read/ota/reset/markvalid/otamon/maclist` (Task 2 dispatch), I2C reboot kept (Task 4). `commit`/`uncommit`/`clear`/`time` are accepted-but-ignored on the root today via I2C; they are logged as "cmd ignorado" until the gateway/Flask define their server-side effect (tracked for the Gateway plan). Gap: none blocking.
- **Placeholder scan:** none — every code step shows full code.
- **Type consistency:** `post_reading_to_flask` new signature `(mac, ch1, ch2, ch3, tensao)` matches header (Task 1) and call site (Task 3). `i2c_macs_mutex`, `i2c_macs`, `i2c_readings`, `MAX_MACS` come from `i2c.h` (unchanged definitions). Topic macros defined in Task 1 are used verbatim in Task 2.
