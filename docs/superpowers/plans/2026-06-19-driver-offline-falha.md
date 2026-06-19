# Driver sem resposta → "Falha" no gateway — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Quando um driver que já respondeu deixa de mandar leitura por 3 ciclos, o root publica o MAC em `mesh/offline` e o gateway marca todos os canais daquele driver como "Falha" (0); apagar o driver da lista some dos dois lados.

**Architecture:** Reaproveita o `rTCounter` por MAC que o root já incrementa a cada broadcast. Um novo flag `online` por MAC garante que só drivers que já responderam sejam reportados, e que a notificação saia **uma única vez** na transição. O root reusa `notify_offline()` → `mesh/offline`. O gateway passa a assinar `mesh/offline` e zera os canais do MAC. A remoção da lista já funciona ponta a ponta (apenas validada).

**Tech Stack:** Root = ESP-IDF (C, `esp-mqtt`); Gateway = PlatformIO/Arduino (C++, `PubSubClient`, ArduinoJson). Broker Mosquitto no Raspberry.

**Spec:** [docs/superpowers/specs/2026-06-19-driver-offline-falha-design.md](../specs/2026-06-19-driver-offline-falha-design.md)

---

## Nota sobre testes (ler antes de começar)

Os dois repos são firmware embarcado **sem harness de teste unitário em host**.
Portanto não há TDD clássico (teste vermelho→verde). Por tarefa, a verificação
automática é **compilar limpo**:

- **Root** (`d:\MICROCONTROLLER_DEV\MSC-WiFi-Mesh-Gateway`): `idf.py build`
- **Gateway** (`D:\MICROCONTROLLER_DEV\MSC-WEBPAGE-PGNSS-DI251829`): `pio run`

A validação **funcional** é por integração (Task 7), com `mosquitto_sub`/`mosquitto_pub`
contra o broker e o display do gateway. Se a toolchain (`idf.py`/`pio`) não estiver
disponível na máquina de execução, registre isso e pare antes do commit da tarefa.

## File Structure

**Root (`MSC-WiFi-Mesh-Gateway`):**
- Modify `main/i2c.h` — novo campo `online` em `i2c_reading_t`.
- Modify `main/mesh_main.c` — RX marca `online=true`; TX detecta transição e publica `mesh/offline`.

**Gateway (`MSC-WEBPAGE-PGNSS-DI251829`):**
- Modify `src/modbus_comm.h` — declara `macSetFault`.
- Modify `src/modbus_comm.cpp` — implementa `macSetFault`.
- Modify `src/mqtt_comm.cpp` — assina `mesh/offline` e roteia no `onMessage`.

**Nenhuma mudança no Flask** (já consome `mesh/offline`).

---

## Fase A — Root: detectar e publicar offline

### Task 1: Campo `online` em `i2c_reading_t`

**Files:**
- Modify: `main/i2c.h:34-40`

- [ ] **Step 1: Adicionar o campo `online` à struct**

Trocar o bloco da struct (linhas 34-40) por:

```c
typedef struct {
    uint8_t ch1;
    uint8_t ch2;
    uint8_t ch3;
    float tensao;
    uint8_t rTCounter;   /* staleness: 0 = leitura fresca, >=3 = offline */
    bool    online;      /* true após a 1ª resposta; false até responder ou após cair */
} i2c_reading_t;
```

Observação: `apply_maclist` ([main/flask_request.c:96-99](../../../main/flask_request.c#L96)) já faz
`memset(&i2c_readings[i], 0, sizeof(...))` ao repor a lista, então `online` nasce
`false` em todo MAC novo/re-adicionado — sem código extra.

- [ ] **Step 2: Compilar**

Run (de `d:\MICROCONTROLLER_DEV\MSC-WiFi-Mesh-Gateway`): `idf.py build`
Expected: build OK (`Project build complete`).

- [ ] **Step 3: Commit**

```bash
git add main/i2c.h
git commit -m "feat(root): campo online por MAC em i2c_reading_t"
```

---

### Task 2: RX marca `online=true` ao receber leitura

**Files:**
- Modify: `main/mesh_main.c:180-190`

- [ ] **Step 1: Setar `online=true` junto com o reset do contador**

No `case BIN_MSG_READ_RESPONSE`, dentro do `if (memcmp(...))`, logo após
`i2c_readings[i].rTCounter = 0;`, inserir a linha `online = true`. O bloco fica:

```c
                        for (int i = 0; i < i2c_mac_count; i++) {
                            if (memcmp(from.addr, i2c_macs[i], 6) == 0) {
                                i2c_readings[i].ch1       = resp->ch1;
                                i2c_readings[i].ch2       = resp->ch2;
                                i2c_readings[i].ch3       = resp->ch3;
                                i2c_readings[i].tensao    = resp->volts / 15.6;   /* atualiza tensão */

                                i2c_readings[i].rTCounter = 0;       /* resposta recebida: fresca */
                                i2c_readings[i].online    = true;    /* já respondeu ao menos 1x */
                                break;
                            }
                        }
```

Observação: este RX escreve `i2c_readings` sem o `i2c_macs_mutex`, igual ao código
atual. Mantemos o padrão existente — `online` é um `bool` único e a escrita é
benigna; não introduzir lock novo (fora do escopo).

- [ ] **Step 2: Compilar**

Run (de `d:\MICROCONTROLLER_DEV\MSC-WiFi-Mesh-Gateway`): `idf.py build`
Expected: build OK.

- [ ] **Step 3: Commit**

```bash
git add main/mesh_main.c
git commit -m "feat(root): marca driver como online ao receber leitura"
```

---

### Task 3: TX publica `mesh/offline` na transição rTCounter→3

**Files:**
- Modify: `main/mesh_main.c:234-244`

- [ ] **Step 1: Coletar MACs em transição sob o mutex e publicar fora dele**

Trocar o bloco atual:

```c
            if (xSemaphoreTake(i2c_macs_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                local_count = i2c_mac_count;
                memcpy(local_macs, i2c_macs, local_count * 6);
                /* Cada ciclo de broadcast conta como uma "tentativa". O contador é
                   zerado quando a resposta chega (ver RX); ao atingir 3 o onRequest
                   passa a reportar leituras zeradas (sensor offline). */
                for (int i = 0; i < local_count; i++) {
                    if (i2c_readings[i].rTCounter < 3) i2c_readings[i].rTCounter++;
                }
                xSemaphoreGive(i2c_macs_mutex);
            }
```

por:

```c
            uint8_t offline_macs[MAX_MACS][6];
            int     offline_count = 0;

            if (xSemaphoreTake(i2c_macs_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                local_count = i2c_mac_count;
                memcpy(local_macs, i2c_macs, local_count * 6);
                /* Cada ciclo de broadcast conta como uma "tentativa"; o contador é
                   zerado quando a resposta chega (ver RX). Na transição para 3, se o
                   driver já tinha respondido (online), marca-o para notificar UMA vez. */
                for (int i = 0; i < local_count; i++) {
                    if (i2c_readings[i].rTCounter < 3) {
                        i2c_readings[i].rTCounter++;
                        if (i2c_readings[i].rTCounter == 3 && i2c_readings[i].online) {
                            i2c_readings[i].online = false;
                            memcpy(offline_macs[offline_count++], i2c_macs[i], 6);
                        }
                    }
                }
                xSemaphoreGive(i2c_macs_mutex);
            }

            /* publica fora da seção crítica (notify_offline enfileira no esp-mqtt) */
            for (int i = 0; i < offline_count; i++) {
                notify_offline(offline_macs[i]);
                ESP_LOGW(MESH_TAG, "[OFFLINE] "MACSTR" nao responde (3x)", MAC2STR(offline_macs[i]));
            }
```

Notas:
- `< 3` antes de incrementar garante que, uma vez saturado em 3, não há
  re-incremento nem re-notificação → publica só na transição 2→3.
- `notify_offline`, `MACSTR`, `MAC2STR`, `MAX_MACS` e `MESH_TAG` já estão em escopo
  neste arquivo (usados em [main/mesh_main.c:250](../../../main/mesh_main.c#L250) e
  [main/mesh_main.c:367](../../../main/mesh_main.c#L367)).
- `notify_offline` publica `mesh/offline` em QoS 1, **não retido**
  ([main/flask_request.c:65-73](../../../main/flask_request.c#L65)) — não deixa flag velha no broker.

- [ ] **Step 2: Compilar**

Run (de `d:\MICROCONTROLLER_DEV\MSC-WiFi-Mesh-Gateway`): `idf.py build`
Expected: build OK.

- [ ] **Step 3: Commit**

```bash
git add main/mesh_main.c
git commit -m "feat(root): publica mesh/offline na transicao de 3 leituras sem resposta"
```

---

## Fase B — Gateway: marcar canais em falha

> Todos os caminhos abaixo são no repo `D:\MICROCONTROLLER_DEV\MSC-WEBPAGE-PGNSS-DI251829`.

### Task 4: `macSetFault(mac)` em modbus_comm

**Files:**
- Modify: `src/modbus_comm.h:85-86`
- Modify: `src/modbus_comm.cpp` (após `removerSensorPorCuba`, ~L419)

- [ ] **Step 1: Declarar a função no header**

Em `src/modbus_comm.h`, junto às outras declarações `void extern ...` (perto da
linha 85-86, depois de `removerSensorPorCuba`), adicionar:

```cpp
void extern macSetFault(String mac);
```

- [ ] **Step 2: Implementar no .cpp**

Em `src/modbus_comm.cpp`, após o fim de `removerSensorPorCuba` (após a chave de
fechamento da função, ~linha 419), adicionar:

```cpp
// Marca todos os canais de um driver como falha (0) quando ele para de responder.
// Recebe o MAC via mesh/offline (ver mqtt_comm.cpp). Espelha a divisao
// sensor/daytank do macFilter, mas varre sensor[] direto (sem reler SPIFFS).
// Recuperacao: a proxima mesh/reading sobrescreve os canais via macFilter.
void macSetFault(String mac)
{
    for (int i = 0; i < indexRemota; i++)
    {
        if (sensor[i].MAC == mac)
        {
            sensor[i].ch1 = 0;
            sensor[i].ch2 = 0;
            sensor[i].ch3 = 0;
        }
        if (sensor[i].MACDT == mac)
        {
            sensor[i].DT = 0;
        }
    }
}
```

Notas:
- `sensor` e `indexRemota` estão definidos neste arquivo
  ([src/modbus_comm.cpp:11](../../../../MSC-WEBPAGE-PGNSS-DI251829/src/modbus_comm.cpp#L11),
  [src/modbus_comm.cpp:13](../../../../MSC-WEBPAGE-PGNSS-DI251829/src/modbus_comm.cpp#L13)).
- `0` = "Falha" no display ([src/main.cpp:507-508](../../../../MSC-WEBPAGE-PGNSS-DI251829/src/main.cpp#L507)).
- O `mac` chega já em maiúsculas (normalizado no `onMessage`, Task 5), casando com
  `sensor[i].MAC` que vem da tabela (também maiúscula).

- [ ] **Step 3: Compilar**

Run (de `D:\MICROCONTROLLER_DEV\MSC-WEBPAGE-PGNSS-DI251829`): `pio run`
Expected: build OK (`SUCCESS`).

- [ ] **Step 4: Commit**

```bash
git add src/modbus_comm.h src/modbus_comm.cpp
git commit -m "feat(gateway): macSetFault zera canais de um driver offline"
```

---

### Task 5: Assinar `mesh/offline` e rotear no `onMessage`

**Files:**
- Modify: `src/mqtt_comm.cpp:6-7` (extern), `:14-17` (tópico), `:19-36` (onMessage), `:43-44` (subscribe)

- [ ] **Step 1: Declarar o extern e o tópico**

Em `src/mqtt_comm.cpp`, ao lado do `extern void macFilter(...)` (linha 7), adicionar:

```cpp
extern void macSetFault(String mac);
```

E na lista de tópicos (linhas 14-17), adicionar:

```cpp
static const char *T_OFFLINE  = "mesh/offline";
```

- [ ] **Step 2: Rotear os dois tópicos no `onMessage`**

Trocar a função `onMessage` inteira (linhas 19-36) por:

```cpp
static void onMessage(char *topic, byte *payload, unsigned int len)
{
    JsonDocument doc;
    if (deserializeJson(doc, payload, len)) return;   // JSON inválido: ignora

    const char *mac = doc["mac"] | "";
    if (!mac[0]) return;
    String macToSend = String(mac);
    macToSend.toUpperCase();

    if (strcmp(topic, T_READING) == 0)
    {
        uint8_t ch1 = doc["ch1"] | 1;
        uint8_t ch2 = doc["ch2"] | 1;
        uint8_t ch3 = doc["ch3"] | 1;
        float tensao = doc["tensao"] | 15.0f;
        macFilter(macToSend, ch1, ch2, ch3, tensao);
    }
    else if (strcmp(topic, T_OFFLINE) == 0)
    {
        macSetFault(macToSend);
    }
}
```

- [ ] **Step 3: Assinar `mesh/offline` no reconnect**

Em `reconnect()`, logo após `s_mqtt.subscribe(T_READING, 1);` (linha 44), adicionar:

```cpp
        s_mqtt.subscribe(T_OFFLINE, 1);
```

- [ ] **Step 4: Compilar**

Run (de `D:\MICROCONTROLLER_DEV\MSC-WEBPAGE-PGNSS-DI251829`): `pio run`
Expected: build OK.

- [ ] **Step 5: Commit**

```bash
git add src/mqtt_comm.cpp
git commit -m "feat(gateway): assina mesh/offline e marca driver em falha"
```

---

## Fase C — Validação de integração

### Task 6: Validar offline + recuperação + remoção (hardware/broker)

**Files:** nenhum (validação manual).

Pré-requisitos: root e gateway gravados com o firmware novo, ambos conectados ao
broker Mosquitto; ao menos um driver cadastrado e respondendo.

- [ ] **Step 1: Monitorar o broker**

Run (no Raspberry/host com acesso ao broker):
```bash
mosquitto_sub -h 192.168.10.190 -t 'mesh/#' -v
```
Expected: `mesh/reading {...}` aparecendo a cada ciclo para o driver vivo; gateway
mostra Vazio/Cheio nos canais.

- [ ] **Step 2: Derrubar o driver e observar o offline**

Desligar/desconectar o driver. Após ~3 ciclos de leitura:
Expected: surge **uma** publicação `mesh/offline {"mac":"<mac-do-driver>"}` no
`mosquitto_sub`; e **não** se repete a cada ciclo seguinte. Display do gateway passa
a "Falha" em S1/S2/S3 (e "Dt: Falha" se o MAC for o do daytank).

- [ ] **Step 3: Confirmar não-republicação**

Aguardar vários ciclos com o driver ainda desligado.
Expected: nenhuma nova `mesh/offline` para o mesmo MAC (só a da transição).

- [ ] **Step 4: Religar o driver e confirmar recuperação**

Religar o driver.
Expected: na próxima `mesh/reading`, o display volta aos valores reais
(Vazio/Cheio) — a falha some sozinha, sem mensagem de "voltou".

- [ ] **Step 5: Apagar o driver da lista e confirmar que some dos dois lados**

Pela página do gateway, apagar a cuba/driver.
Expected: some do display do gateway; no `mosquitto_sub` aparece `mesh/cmd/maclist`
(retido) com a lista menor; o root para de emitir `mesh/reading`/`mesh/offline` para
aquele MAC (deixou de ser pollado).

- [ ] **Step 6: Registrar resultado**

Anotar no PR/issue o resultado de cada passo. Se algum falhar, voltar ao spec antes
de ajustar o código.

---

## Self-Review (preenchido pelo autor do plano)

**Cobertura do spec:**
- A) Root detecta e publica offline → Tasks 1-3. ✔
- B) Gateway marca falha → Tasks 4-5. ✔
- C) Remoção some dos dois lados → sem código (já funciona); validado na Task 6 Step 5. ✔
- Semântica "só quem já respondeu" + "uma vez" → flag `online` (Tasks 1-3). ✔
- Recuperação sem mensagem → comportamento do `macFilter` (validado Task 6 Step 4). ✔
- Flask sem mudança → confirmado, nenhuma task. ✔

**Placeholders:** nenhum TBD/TODO; todo passo de código mostra o código completo.

**Consistência de tipos/nomes:** `online` (bool) usado igual em i2c.h/RX/TX;
`macSetFault(String)` declarado em modbus_comm.h, definido em modbus_comm.cpp,
chamado em mqtt_comm.cpp; `T_OFFLINE` = `"mesh/offline"` casa com `TOPIC_OFFLINE`
do root ([main/flask_request.h:17](../../../main/flask_request.h#L17)).
