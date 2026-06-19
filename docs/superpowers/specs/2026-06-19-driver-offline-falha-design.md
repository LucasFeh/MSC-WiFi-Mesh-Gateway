# Driver que para de responder → "Falha" no gateway

**Data:** 2026-06-19
**Status:** Aprovado (aguardando revisão do spec → plano de implementação)
**Repos afetados:** `MSC-WiFi-Mesh-Gateway` (root), `MSC-WEBPAGE-PGNSS-DI251829` (gateway)

## Contexto

Quando um driver desliga ou perde conexão e para de enviar leituras, hoje ele
continua aparecendo "ativo" no gateway, com a **última leitura congelada**. Não
existe nenhum tratamento que transforme essa ausência de resposta em "Falha":

- **Root:** o `rTCounter` por MAC é incrementado a cada ciclo de broadcast
  (saturando em 3) e zerado quando chega resposta
  ([main/mesh_main.c:240-242](../../../main/mesh_main.c#L240), [main/mesh_main.c:187](../../../main/mesh_main.c#L187)),
  mas **nada acontece ao atingir 3** — o consumidor era o `onRequest` do I2C,
  removido na migração para MQTT. O `notify_offline()` → `mesh/offline`
  ([main/flask_request.c:65-73](../../../main/flask_request.c#L65)) só é chamado em
  `MESH_EVENT_CHILD_DISCONNECTED` ([main/mesh_main.c:367](../../../main/mesh_main.c#L367)),
  que cobre só filho direto do root caindo da mesh — não o driver que simplesmente
  parou de responder, nem nó em camada mais profunda.
- **Gateway:** assina **só** `mesh/reading` ([mqtt_comm.cpp:44](../../../../MSC-WEBPAGE-PGNSS-DI251829/src/mqtt_comm.cpp#L44)).
  Sem leitura, o callback não dispara, `macFilter` não roda e `sensor[]` fica
  congelado. Não há timeout/watchdog local. Os defaults `doc["ch1"] | 1` só valem
  para campo faltando numa mensagem recebida e caem em `1` ("Vazio"), nunca em `0`.

Codificação dos canais no display ([main.cpp:505-589](../../../../MSC-WEBPAGE-PGNSS-DI251829/src/main.cpp#L505)):
`0 = "Falha"`, `1 = "Vazio"`, `2 = "Cheio"`.

## Objetivo

Quando um driver **que já enviou ao menos uma leitura** deixar de responder por
**3 requisições** consecutivas, o root publica o MAC em `mesh/offline`; o gateway
recebe e marca **todos os canais** daquele driver como falha (`0`). Ao apagar o
driver da lista de MACs, ele some do gateway **e** do root.

## Não-objetivos

- Flask: nenhuma mudança. Já consome `mesh/offline`
  ([server/app.py:221-227](../../../server/app.py#L221)).
- Mensagem explícita de "voltou online": desnecessária. A próxima `mesh/reading`
  sobrescreve os canais com os valores reais e a falha some sozinha.
- Mismatch de IP do broker do gateway (`192.168.15.191`) vs root/Flask
  (`192.168.10.190`): apenas heads-up, fora do escopo.

## Arquitetura / fluxo

```
driver para de responder
        │  (3 ciclos sem resposta, rTCounter==3, online==true)
        ▼
ROOT  notify_offline(mac) ──► mesh/offline {"mac":...}  (QoS 1, NÃO retido)
        │
        ▼
GATEWAY  onMessage → macSetFault(mac) → sensor[*].ch=0 ("Falha" no display)
        │
        ▼  (driver volta) mesh/reading → macFilter → canais reais (falha some)
```

## A) Root — detectar e publicar offline

**Reaproveita o `rTCounter` existente (não cria task nova).**

- **[main/i2c.h](../../../main/i2c.h)** — novo campo em `i2c_reading_t`:
  ```c
  bool online;   /* true após a 1ª resposta; false até responder ou após cair */
  ```
- **RX** (`BIN_MSG_READ_RESPONSE`, [main/mesh_main.c:180-190](../../../main/mesh_main.c#L180)):
  ao gravar a leitura fresca, setar `i2c_readings[i].online = true` (além de
  `rTCounter = 0`).
- **TX** (loop de incremento, [main/mesh_main.c:240-242](../../../main/mesh_main.c#L240)):
  detectar a transição para `rTCounter == 3` **com `online == true`**. Ainda sob
  o `i2c_macs_mutex`, marcar o índice (ou copiar o MAC) e setar `online = false`.
  **Após soltar o mutex**, chamar `notify_offline(mac)` para cada índice marcado
  (não publicar dentro da seção crítica).

**Semântica resultante:**

| Situação | `online` | Publica `mesh/offline`? |
|---|---|---|
| MAC recém-adicionado (`memset` do `apply_maclist` zera) | `false` | Não — nunca respondeu |
| Respondeu ao menos 1×, depois 3 ciclos sem resposta | `true → false` | **Sim, uma vez** na transição |
| Continua sem responder (rTCounter já saturado em 3) | `false` | Não (não republica) |
| Volta a responder | `false → true` | Não (a `mesh/reading` recupera) |

`mesh/offline` continua **QoS 1, não retido** (igual ao `notify_offline` atual):
evita deixar flag de offline velha grudada no broker.

## B) Gateway — marcar falha

- **[mqtt_comm.cpp](../../../../MSC-WEBPAGE-PGNSS-DI251829/src/mqtt_comm.cpp)**:
  - `reconnect()`: assinar também `mesh/offline` (QoS 1).
  - `onMessage`: rotear por tópico — `mesh/reading` → `macFilter` (como hoje);
    `mesh/offline` → `macSetFault(mac)`. MAC normalizado em maiúsculas, como já é
    feito para `mesh/reading`.
- **[modbus_comm.cpp](../../../../MSC-WEBPAGE-PGNSS-DI251829/src/modbus_comm.cpp) + [.h](../../../../MSC-WEBPAGE-PGNSS-DI251829/src/modbus_comm.h)**:
  nova `void macSetFault(String mac)` — varre `sensor[0..indexRemota)` **direto**
  (sem reler o SPIFFS): onde `sensor[i].MAC == mac` → `ch1 = ch2 = ch3 = 0`; onde
  `sensor[i].MACDT == mac` → `DT = 0`. Espelha a divisão sensor/daytank do
  `macFilter`.

## C) Remoção da lista de MACs — sem código novo

Já funciona ponta a ponta ([asyncWeb.cpp:180-184](../../../../MSC-WEBPAGE-PGNSS-DI251829/src/asyncWeb.cpp#L180)):
`removeObjectBykey` → `parsearConexoes()` (reconstrói `uniqueMacs` sem o MAC) →
`removerSensorPorCuba()` (tira do `sensor[]`/display) → `commmit()` →
`mqttPublishMaclist()` republica a lista retida menor. O root recebe em
`apply_maclist` ([main/flask_request.c:84-103](../../../main/flask_request.c#L84)),
**substitui a lista inteira** e zera as leituras (`online` volta a `false`); o MAC
removido deixa de ser pollado → some do root. Apenas **validar** no rollout.

## Casos de borda

- **Reboot do root:** perde os flags `online` (todos `false`); recupera a lista via
  `mesh/cmd/maclist` retido; cada driver volta a `online=true` na 1ª resposta. Não
  há offline falso durante a lacuna (a transição exige `online==true`).
- **Publicação sob mutex:** os MACs a notificar são coletados dentro da seção
  crítica e publicados **fora** dela.
- **Falha auto-limpa:** voltar a responder não precisa de mensagem própria; a
  `mesh/reading` sobrescreve os canais via `macFilter`.

## Testes / rollout

1. Cadastrar driver, receber leituras (gateway mostra Vazio/Cheio).
2. Desligar o driver → após ~3 ciclos de leitura, root publica `mesh/offline`
   (verificável com `mosquitto_sub -t 'mesh/#'`).
3. Gateway mostra **"Falha"** em S1/S2/S3 (e Dt, se for o MAC do daytank).
4. Religar o driver → próxima `mesh/reading` → canais voltam aos valores reais.
5. Apagar a cuba → driver some do display do gateway e o root para de pollá-lo.
6. Confirmar que `mesh/offline` **não** republica a cada ciclo (só na transição).
