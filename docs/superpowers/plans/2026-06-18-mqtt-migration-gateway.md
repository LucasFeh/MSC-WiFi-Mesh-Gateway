# MQTT Migration — GATEWAY (plan 3 of 3) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the gateway's I2C master (readings + commands) with an MQTT client: subscribe to `mesh/reading` (feeding `macFilter()` continuously), publish commands to `mesh/cmd/*`, remove all dead/unused I2C commands, and keep only the I2C reboot-of-root (`UPDT_I2C`).

**Architecture:** A `PubSubClient` MQTT client (lib already vendored) runs over the gateway's active network interface (Ethernet/W5500, lwIP). A new `src/mqtt_comm.{h,cpp}` centralizes connect/reconnect/loop, the `mesh/reading` subscription (callback → `macFilter`), and the command publishers. The 20 s timer-driven action loop stays, but `slaveRequest()` polling is removed — `sensor[]` is kept fresh asynchronously by incoming MQTT. The only surviving I2C is `UpdateI2C()` → `"UPDT_I2C"` (reboot of root).

**Tech Stack:** PlatformIO / Arduino-ESP32 (`esp32dev`), `PubSubClient`, `ArduinoJson` (both vendored in `.pio/libdeps`).

**Repo:** `D:\MICROCONTROLLER_DEV\MSC-WEBPAGE-PGNSS-DI251829`. Contract = `docs/superpowers/specs/2026-06-18-i2c-to-mqtt-migration-design.md` (in the gateway repo, that file lives in the root repo; topics are reproduced inline here).

---

## Config gotcha (must resolve before testing)

The gateway's Ethernet IP defaults to `192.168.1.114` (prefs `config/ethernetIp`); the broker is `192.168.15.191`. **They must be on the same reachable network.** Either put the broker on a reachable IP, or set the gateway's IP to the `192.168.15.x` subnet. `MQTT_BROKER_HOST` below must be the broker IP reachable from the gateway.

---

## File Structure

- Create `src/mqtt_comm.h` / `src/mqtt_comm.cpp` — MQTT client: init/reconnect/loop, `mesh/reading` subscribe→`macFilter`, command publishers.
- Modify `src/main.cpp` — init MQTT in `setup()`; call `mqttCommLoop()` in `loop()`; remove the `slaveRequest()` call.
- Modify `src/modbus_comm.cpp` / `src/modbus_comm.h` — `commmit()` publishes `mesh/cmd/maclist`; `CommitSensor()` publishes `mesh/cmd/read`; `SendTimer()` publishes `mesh/cmd/time`; delete `slaveRequest`, `slaveRequestTCP`, and the dead command senders; keep `UpdateI2C()` (I2C reboot).
- Modify `src/asyncWeb.cpp` — the inline `CLICKED` I2C write → `mqttPublishRead()`; remove UI triggers of deleted commands.
- Modify `src/utils.cpp` — the `CLICKED` I2C write (line ~624) → `mqttPublishRead()`.
- Verify `platformio.ini` lists `knolleary/PubSubClient` and `bblanchon/ArduinoJson` in `lib_deps`.

Functional command set kept (root acts on these): **maclist**, **read**, **reboot-of-root via I2C**. `time` is published too (root-side handling is a cross-plan confirm item, see Task 6). Everything the root ignores today (`UPDATE`, `OPDATEUNICAST*`, `RBOT_I2C`, `CLEAR`, `REBOOT`-broadcast, `UNCOMMIT`) is **removed**.

---

## Task 1: MQTT client module (connect + subscribe readings → macFilter)

**Files:**
- Create: `src/mqtt_comm.h`
- Create: `src/mqtt_comm.cpp`

- [ ] **Step 1: Create `src/mqtt_comm.h`**

```cpp
#ifndef MQTT_COMM_H
#define MQTT_COMM_H

#include <Arduino.h>
#include <vector>

// Broker MQTT no Raspberry (mesmo do Flask/root). DEVE ser alcançável da
// interface de rede do gateway (ver "Config gotcha" no plano).
#define MQTT_BROKER_HOST   "192.168.15.191"
#define MQTT_BROKER_PORT   1883

void mqttCommInit();                       // chamar no setup() após a rede subir
void mqttCommLoop();                       // chamar no loop() (mantém conexão + entrega msgs)
void mqttPublishMaclist(const std::vector<String> &macs);
void mqttPublishRead();
void mqttPublishTime(int seconds);

#endif // MQTT_COMM_H
```

- [ ] **Step 2: Create `src/mqtt_comm.cpp`**

```cpp
#include "mqtt_comm.h"
#include <WiFi.h>            // WiFiClient roda sobre a interface lwIP ativa (Ethernet/W5500)
#include <PubSubClient.h>
#include <ArduinoJson.h>

// Definidas em modbus_comm.cpp / utils — atualiza a tabela sensor[] a partir da leitura.
extern void macFilter(String mac, uint8_t CH1, uint8_t CH2, uint8_t CH3, float tensao);

static WiFiClient   s_net;
static PubSubClient s_mqtt(s_net);

static const char *T_READING  = "mesh/reading";
static const char *T_MACLIST  = "mesh/cmd/maclist";
static const char *T_READ     = "mesh/cmd/read";
static const char *T_TIME     = "mesh/cmd/time";

static void onMessage(char *topic, byte *payload, unsigned int len)
{
    if (strcmp(topic, T_READING) != 0) return;

    JsonDocument doc;
    if (deserializeJson(doc, payload, len)) return;   // JSON inválido: ignora

    const char *mac = doc["mac"] | "";
    if (!mac[0]) return;
    uint8_t ch1 = doc["ch1"] | 1;
    uint8_t ch2 = doc["ch2"] | 1;
    uint8_t ch3 = doc["ch3"] | 1;
    float tensao = doc["tensao"] | 15.0f;

    String macToSend = String(mac);
    macToSend.toUpperCase();
    macFilter(macToSend, ch1, ch2, ch3, tensao);
}

static void reconnect()
{
    if (s_mqtt.connected()) return;
    // clientId único; LWT não é necessário no gateway (consumidor).
    String cid = "gateway-" + String((uint32_t)ESP.getEfuseMac(), HEX);
    if (s_mqtt.connect(cid.c_str())) {
        s_mqtt.subscribe(T_READING, 1);
        Serial.println("[MQTT] conectado, assinando mesh/reading");
    }
}

void mqttCommInit()
{
    s_mqtt.setServer(MQTT_BROKER_HOST, MQTT_BROKER_PORT);
    s_mqtt.setBufferSize(512);          // payload de leitura cabe folgado
    s_mqtt.setCallback(onMessage);
    reconnect();
}

void mqttCommLoop()
{
    if (!s_mqtt.connected()) reconnect();
    s_mqtt.loop();
}

void mqttPublishMaclist(const std::vector<String> &macs)
{
    // {"macs":["aa:..","bb:.."]} — retained, para o root recuperar após reboot.
    JsonDocument doc;
    JsonArray arr = doc["macs"].to<JsonArray>();
    for (auto &m : macs) arr.add(m);
    char buf[1024];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    s_mqtt.publish(T_MACLIST, (const uint8_t *)buf, n, true);   // retain=true
    Serial.printf("[MQTT] maclist publicada (%u MAC[s])\n", (unsigned)macs.size());
}

void mqttPublishRead()
{
    s_mqtt.publish(T_READ, "{}");
    Serial.println("[MQTT] read publicado");
}

void mqttPublishTime(int seconds)
{
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "{\"seconds\":%d}", seconds);
    s_mqtt.publish(T_TIME, (const uint8_t *)buf, n, false);
}
```

- [ ] **Step 3: Confirm `platformio.ini` deps**

Open `platformio.ini`; ensure `lib_deps` contains `knolleary/PubSubClient` and `bblanchon/ArduinoJson`. If `PubSubClient` is missing, add it under `lib_deps`. (Both are already in `.pio/libdeps`, so they are almost certainly declared — just verify.)

- [ ] **Step 4: Build**

Run: `pio run -e esp32dev`
Expected: compiles (the module isn't called yet; this verifies the new files build).

- [ ] **Step 5: Commit**

```bash
git add src/mqtt_comm.h src/mqtt_comm.cpp platformio.ini
git commit -m "feat(gateway): módulo MQTT (assina mesh/reading -> macFilter) + publishers"
```

---

## Task 2: Wire MQTT into setup()/loop(); stop polling I2C readings

**Files:**
- Modify: `src/main.cpp`

- [ ] **Step 1: Include the module**

At the top of `src/main.cpp` (with the other includes), add:
```cpp
#include "mqtt_comm.h"
```

- [ ] **Step 2: Init MQTT after the network is up**

In `setup()`, immediately after `initWebServer();` (inside the `if (ESP32_W5500_isConnected())` block, around line 245), add:
```cpp
        mqttCommInit();
```

- [ ] **Step 3: Drive the MQTT client from loop() and remove the I2C poll**

In `loop()`, replace the `slaveRequest();` call (line ~306) with nothing (delete it), and add `mqttCommLoop();` at the very start of `loop()` so messages flow every iteration. Concretely, change the start of `loop()`:
```cpp
void loop()
{
    mqttCommLoop();

    if (timerFlag)
    {
```
and delete this line further down:
```cpp
        slaveRequest();
```

- [ ] **Step 4: Build**

Run: `pio run -e esp32dev`
Expected: compiles. (`sensor[]` is now fed by MQTT; the 20 s timer still drives `receive_now(...)` over `sensor[]`.)

- [ ] **Step 5: Commit**

```bash
git add src/main.cpp
git commit -m "feat(gateway): MQTT no setup/loop; remove polling slaveRequest()"
```

---

## Task 3: Publish maclist + read instead of I2C writes

**Files:**
- Modify: `src/modbus_comm.cpp`
- Modify: `src/asyncWeb.cpp`
- Modify: `src/utils.cpp`

- [ ] **Step 1: `commmit()` publishes the MAC list (modbus_comm.cpp)**

In `src/modbus_comm.cpp`, replace the MAC-send loop in `commmit()` (the `for (auto &m : uniqueMacs) { Wire.beginTransmission(...) ... }` block, ~lines 177-186) plus its trailing `SendTimer();` with a single publish. The function keeps building `uniqueMacs` exactly as before; only the transmission changes:
```cpp
    customSerialPrint("------------------------------------------------------------");
    customSerialPrint("Publicando lista de MACs via MQTT...");

    mqttPublishMaclist(uniqueMacs);
    SendTimer();
```
Add `#include "mqtt_comm.h"` at the top of `src/modbus_comm.cpp`.

- [ ] **Step 2: `CommitSensor()` publishes a read trigger (modbus_comm.cpp)**

Replace the body of `CommitSensor()` (~lines 203-209) with:
```cpp
void CommitSensor()
{
    // Dispara uma leitura na mesh (antes: I2C "COMMIT").
    mqttPublishRead();
}
```

- [ ] **Step 3: `SendTimer()` publishes the interval (modbus_comm.cpp)**

In `SendTimer()`, replace the final I2C block (~lines 336-339):
```cpp
    Wire.beginTransmission(SLAVE_ADDR);
    Wire.write("TIME:");
    Wire.write(timerinmillis);
    Wire.endTransmission();
```
with:
```cpp
    mqttPublishTime(atoi(timerinmillis));
```

- [ ] **Step 4: Replace the inline CLICKED writes with a read publish**

In `src/asyncWeb.cpp` (~lines 120-122) replace:
```cpp
        Wire.beginTransmission(SLAVE_ADDR);
        Wire.write("CLICKED");
        Wire.endTransmission();
```
with:
```cpp
        mqttPublishRead();
```
Add `#include "mqtt_comm.h"` at the top of `src/asyncWeb.cpp`.

In `src/utils.cpp` (~lines 624-626) replace the same three `Wire` lines (`"CLICKED"`) with:
```cpp
    mqttPublishRead();
```
Add `#include "mqtt_comm.h"` at the top of `src/utils.cpp`.

- [ ] **Step 5: Build**

Run: `pio run -e esp32dev`
Expected: compiles.

- [ ] **Step 6: Commit**

```bash
git add src/modbus_comm.cpp src/asyncWeb.cpp src/utils.cpp
git commit -m "feat(gateway): publica maclist/read/time via MQTT (substitui writes I2C)"
```

---

## Task 4: Remove dead I2C commands; keep only reboot-of-root

**Files:**
- Modify: `src/modbus_comm.cpp` / `src/modbus_comm.h`
- Modify: `src/asyncWeb.cpp`

- [ ] **Step 1: Delete the dead command senders (modbus_comm.cpp)**

Delete these functions entirely (the root ignores all of them today): `descommmit`, `UpdateSensor`, `UpdateSensorSTA`, `RebootDriver`, `RebootI2C`, `UpdateI2CSTA`, `UpdateUniCastAP`, `UpdateUniCastSTA`, `UncommitSensor`, `clearAllMacs`, `slaveRequest`, `slaveRequestTCP`. **Keep** `UpdateI2C()` (the `"UPDT_I2C"` reboot-of-root over I2C — the one surviving I2C command) and `commmit`/`CommitSensor`/`SendTimer`/`macFilter`/`receive_now`/the Modbus helpers.

- [ ] **Step 2: Remove their declarations (modbus_comm.h)**

Delete the matching `void extern ...;` prototypes for every function removed in Step 1. Keep `slaveRequest`? No — remove `void extern slaveRequest();` and `void extern slaveRequestTCP();` too. Keep `commmit`, `CommitSensor`, `SendTimer`, `macFilter`, `receive_now`, `UpdateI2C`, `parsearConexoes`, `limparSensores`, `printConexoes`, `criarJsonSinalizadores`, `send_modbus*`, `preTransmission`, `postTransmission`.

- [ ] **Step 3: Remove UI triggers of deleted commands (asyncWeb.cpp)**

Find and remove the web-handler calls to deleted functions: `clearAllMacs()` (~line 185) — drop the call, keep the following `commmit()`; `UncommitSensor()` (~line 234) — remove that handler branch (UNCOMMIT was a no-op on the root). Leave `commmit()`, `CommitSensor()`, `SendTimer()`, and `UpdateI2C()` (~line 295) calls intact. Grep to confirm no dangling references remain:
```
grep -n "clearAllMacs\|UncommitSensor\|RebootDriver\|RebootI2C\|UpdateSensor\|UpdateUniCast\|UpdateI2CSTA\|descommmit\|slaveRequest" src/
```
Expected: only `UpdateI2C()` (without the `STA` suffix) and the kept functions remain; the listed dead names return nothing.

- [ ] **Step 4: Build**

Run: `pio run -e esp32dev`
Expected: compiles with no undefined references.

- [ ] **Step 5: Commit**

```bash
git add src/modbus_comm.cpp src/modbus_comm.h src/asyncWeb.cpp
git commit -m "refactor(gateway): remove comandos I2C mortos; mantém só UPDT_I2C (reboot root)"
```

---

## Task 5: Integration test against the broker

**Files:** none (runtime verification)

- [ ] **Step 1: Set a reachable broker IP**

Confirm the gateway can reach `192.168.15.191:1883` from its Ethernet interface (adjust `MQTT_BROKER_HOST` and/or the gateway IP per the "Config gotcha"). Start Mosquitto on the Pi.

- [ ] **Step 2: Flash and watch connect**

Run: `pio run -e esp32dev -t upload -t monitor`
Expected serial: `[MQTT] conectado, assinando mesh/reading`.

- [ ] **Step 3: Confirm maclist + read are published**

On a PC: `mosquitto_sub -h 192.168.15.191 -t 'mesh/cmd/#' -v`. On gateway boot (setup calls `commmit()` then `CommitSensor()`), expect a retained `mesh/cmd/maclist {"macs":[...]}` and a `mesh/cmd/read {}`. Trigger the web "read"/CLICKED button → another `mesh/cmd/read {}`.

- [ ] **Step 4: Confirm readings reach macFilter**

Inject a reading: `mosquitto_pub -h 192.168.15.191 -t mesh/reading -m '{"mac":"aa:bb:cc:dd:ee:01","ch1":5,"ch2":6,"ch3":7,"tensao":3.30}'`.
Expected: with `LOG_I2C` on, serial shows the MAC/CH/tensão line (now from MQTT), and the value appears on the display/Modbus action on the next 20 s tick.

- [ ] **Step 5: Confirm reboot-of-root still works over I2C**

Trigger the web action that calls `UpdateI2C()` (~asyncWeb.cpp:295). Expected: the root logs `[I2C] UPDT_I2C -> esp_restart()` and reboots. (I2C stays wired; `Wire.begin` in `setup()` is unchanged.)

---

## Task 6: Cross-plan confirm — `mesh/cmd/time` on the root

**Files:** (root repo) `main/flask_request.c`, `main/mesh_main.c` — only if the timer is a live feature

- [ ] **Step 1: Decide if the timer is functional**

In the root repo, inspect the `read_timer` task (`mesh_main.c`) and whether `seconds`/`timerAdjust` (set by the old I2C `TIME:` path) are actually consumed to change the read-broadcast interval.
- If **yes**: add a `mesh/cmd/time` case to the root's `on_command` (in `main/flask_request.c`, alongside `mesh/cmd/read`) that parses `{"seconds":N}` and applies it the same way the old `TIME:` did. The gateway already publishes it (`mqttPublishTime`).
- If **no** (vestigial): remove `mqttPublishTime`/the `SendTimer` publish from the gateway and drop the timer UI — it's dead too.

- [ ] **Step 2: Commit the decision** in the affected repo with a message stating which branch was taken.

---

## Self-Review

- **Spec coverage (GATEWAY rows):** subscribe `mesh/reading` → `macFilter` (Task 1); publish `mesh/cmd/maclist` retained (Task 3 Step 1), `mesh/cmd/read` (Task 3 Steps 2/4), `mesh/cmd/time` (Task 3 Step 3, pending Task 6); I2C reduced to reboot-of-root `UPDT_I2C` only (Task 4 keeps `UpdateI2C`, removes the rest). `mesh/cmd/reset`/`markvalid`/`ota` are **Flask-only** controls (dashboard) and intentionally not published by the gateway — no gap.
- **Placeholder scan:** none — full code for the new module; edits reference exact functions/line regions confirmed by grep.
- **Type consistency:** `macFilter(String,uint8_t,uint8_t,uint8_t,float)` matches the existing signature (`modbus_comm.cpp:342`); `mqttPublishMaclist(const std::vector<String>&)` matches `uniqueMacs` (`std::vector<String>`, `modbus_comm.cpp:6`); topic strings match the spec and the ROOT plan's `on_command` (`mesh/cmd/read|maclist`). `UpdateI2C()` (kept) is distinct from the removed `UpdateI2CSTA()`.
- **Open item:** `mesh/cmd/time` root-side handling (Task 6) — explicitly deferred with both branches specified.
```
