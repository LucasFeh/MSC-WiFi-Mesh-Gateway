# I2C Slave → Master Response (onRequest) — Design

**Data:** 2026-06-01
**Alvo:** ESP32 (ESP-IDF, API legada `driver/i2c.h`), modo slave, addr `0x08`, SDA=21 / SCL=22
**Arquivos:** `main/i2c.h`, `main/i2c.c`, `main/mesh_main.c`

## Problema

O caminho **master → slave** (master escreve comandos, slave recebe) já está
implementado e idiomático em `i2c_slave_task()` ([i2c.c]). Falta o caminho oposto:
quando o **master faz um read** (`Wire.requestFrom`), o slave precisa **responder**
com a lista de MACs e suas leituras de mesh (ch1/ch2/ch3). É o equivalente ao
`onRequest()` do firmware Arduino original, ainda não portado para este projeto.

Pistas no código que confirmam o gap:
- `RequestSendFlag` é **setada** por `CLICKED`/`COMMIT`/`UNCOMMIT` mas **nunca lida**
  em lugar nenhum do firmware (confirmado por grep em `main/`).
- Comentário em `i2c.c` diz que `i2c_on_receive()` é o porte do `onReceive()` Arduino
  — o par `onRequest()` está faltando.
- Comentário em `i2c.c` admite que `SensorData[]` "não existe neste projeto".

## Protocolo do master (fixo, não pode mudar)

Extraído do firmware real do master (`slaveRequest()`):

```cpp
bytesReceived = Wire.requestFrom(SLAVE_ADDR, 72);   // sempre 72 bytes por read
Wire.readBytes(temp, bytesReceived);
for (i ...) if (temp[i] != 0xFF && temp[i] != 0x8f) received += (char)temp[i]; // limpa padding
if (received == "Vazio") commit();
else if (received == "FIM") break;                  // encerra o loop
else deserializeJson(doc, received);                // 1 JSON por read
```

Fatos que governam o design:
1. **N = 72 bytes por `requestFrom`.** O stride de cada registro deve ser exatamente 72.
2. **O master descarta `0xFF` e `0x8f`.** Podemos fazer padding com `0xFF`; o master limpa.
3. **Um registro por read:** um JSON, ou `"Vazio"`, ou `"FIM"`.
4. O master itera `requestFrom` em loop, **para em `"FIM"`**, chama `commit()` em `"Vazio"`.
5. O master **tolera** reads vazios (`length()==0` → pula) e JSON inválido (loga e segue),
   o que dá robustez contra corrida na primeira leitura.

JSON esperado (mantendo todos os campos; `tensao` não existe no protocolo mesh → `0`):
```
{"mac":"aa:bb:cc:dd:ee:ff","ch1":N,"ch2":N,"ch3":N,"tensao":0}
```
Pior caso (canais com 3 dígitos): **68 bytes** < 72 → cabe com ≥4 bytes de padding `0xFF`.
MAC em hex minúsculo (`%02x`); o master faz `toUpperCase()` por conta própria.

## Constraint central e solução

O driver **legado** `driver/i2c.h` **não tem callback `onRequest`** (ao contrário do
Arduino, onde `onRequest()` dispara síncrono por read). Em vez disso, pré-carrega-se um
**ring buffer TX** com `i2c_slave_write_buffer()`; o hardware drena conforme o master lê.
O ring buffer é **FIFO e persiste entre transações de read**.

**Importante:** como não há callback de read, **é impossível imprimir algo "a cada
onRequest"** — os reads do master são servidos pelo hardware sem notificar o software.

**O master faz poll contínuo** (loop de `requestFrom` sem escrever antes; o `onRequest`
Arduino original também não dependia de `RequestSendFlag`). Logo, a resposta NÃO pode ser
gatilhada por `RequestSendFlag` — ela precisa estar **sempre disponível**.

**Solução (streaming por FIFO com stride fixo + reabastecimento por-registro):** uma task
dedicada (`i2c_slave_request_task`) monta o blob atual — **todos** os registros, **cada um
com padding `0xFF` até exatos 72 bytes**, terminando com `"FIM"` (lista vazia → `"Vazio"` +
`"FIM"`) — e o entrega ao ring buffer TX **um registro por vez**. Cada `requestFrom(72)`
drena 72 bytes do FIFO → o master recebe **1 registro por read**, avançando sozinho. A
paginação do `sendWireIndex` do Arduino vira o **avanço natural do ponteiro do FIFO**.

**Profundidade do FIFO é crítica:** o ring buffer TX é dimensionado em poucos REGISTROS
(`I2C_TX_DEPTH_RECORDS`, ex.: 8). A escrita por-registro **bloqueia quando cheia**
(backpressure fino, sem busy-wait) e limita o atraso a ~`I2C_TX_DEPTH_RECORDS` leituras —
**independente do tamanho da lista**. Um ring buffer GRANDE relativo ao blob acumularia
muitas cópias velhas na fila (no boot, ~`buffer/blob` cópias de `"Vazio"`), fazendo o master
ler `"Vazio"` por vários segundos antes de alcançar os dados frescos — bug observado e
corrigido. Cada escrita de 72 bytes é atômica no ring buffer, preservando o alinhamento.

Migrar para o driver novo `driver/i2c_slave.h` (que tem callback `on_request`) foi
**descartado**: o projeto proíbe esse driver explicitamente (comentário em `i2c.c`).

## Componentes

### 1. `i2c.h`
- `#define I2C_REQUEST_STRIDE 72` — bytes por registro (= N do master).
- Aumentar `I2C_TX_BUF_SIZE` para `((MAX_MACS + 1) * I2C_REQUEST_STRIDE)` (= 35×72 = 2520;
  arredondar para 2560) — comporta todos os MACs + `FIM`.
- Novo store de leituras, paralelo a `i2c_macs[]` (mesmo índice):
  ```c
  typedef struct { uint8_t ch1, ch2, ch3, tensao, rTCounter; } i2c_reading_t;
  extern i2c_reading_t i2c_readings[MAX_MACS];
  ```

### 2. `i2c.c` — store + onRequest
- Definir `i2c_reading_t i2c_readings[MAX_MACS]` (zerado).
- **`i2c_build_request_blob()`** (estático): sob `i2c_macs_mutex`, para cada índice
  `0..i2c_mac_count-1` monta o JSON (aplicando staleness, ver §4), faz padding `0xFF`
  até `I2C_REQUEST_STRIDE`; acrescenta `"FIM"` paddeado. Lista vazia → `"Vazio"` + `"FIM"`.
  Retorna o tamanho total no buffer estático do módulo.
- **`i2c_load_request_response()`** (estático): chama o build, depois um único
  `i2c_slave_write_buffer(I2C_SLAVE_PORT, blob, len, timeout)`; loga bytes carregados.
- **`i2c_slave_request_task()` (task dedicada, criada no `app_main`)**: loop infinito que
  reconstrói o blob e o escreve inteiro no ring buffer TX com `i2c_slave_write_buffer`
  (timeout finito; bloqueio natural quando cheio). Log periódico (a cada 10 ciclos) confirma
  a atividade slave→master. `RequestSendFlag` deixa de gatilhar a resposta (poll é contínuo).
- Comentários explicando o fluxo slave→master e que **ACK/NACK é feito por hardware**
  no driver legado (nenhuma ação manual necessária).

### 3. `mesh_main.c` — popular leituras
- Em `esp_mesh_p2p_rx_main()`, no case `BIN_MSG_READ_RESPONSE`: sob `i2c_macs_mutex`,
  localizar `from.addr` em `i2c_macs[]`; se achar, gravar `ch1/ch2/ch3`, `tensao=0`,
  `rTCounter=0` (resposta recebida → não está stale). Mantém o `post_reading_to_flask`.

### 4. Staleness `rTCounter` (fiel ao Arduino)
- Em `esp_mesh_p2p_tx_main()`, no loop de broadcast `READ_REQUEST` por MAC
  ([mesh_main.c]): sob mutex, incrementar `i2c_readings[i].rTCounter` (cap em 3) por ciclo.
- No build do JSON (§2): se `rTCounter >= 3` → enviar `ch1=ch2=ch3=0, tensao=0`
  (sensor considerado offline), igual ao `if (rTCounter == 3)` do Arduino.
- A resposta da mesh (§3) reseta `rTCounter=0`.

## Fluxo de dados

```
i2c_slave_request_task (loop)  -> build blob (macs + i2c_readings, padding 0xFF/72 + FIM)
                                -> i2c_slave_write_buffer (TX ring FIFO; bloqueia se cheio)
Master  --requestFrom(72) xN-->  HW drena 72B/registro do FIFO (contínuo)
                                  -> master limpa 0xFF/0x8f, parseia 1 JSON por read
                                  -> para em "FIM", reinicia o poll
Master  --escreve "CLICKED"-->  i2c_slave_task -> i2c_on_receive (comandos; flags de controle)

mesh node --READ_RESPONSE-->  esp_mesh_p2p_rx_main -> grava i2c_readings[idx] (mutex)
broadcast  READ_REQUEST    ->  esp_mesh_p2p_tx_main -> rTCounter++ (mutex, cap 3)
```

## Tratamento de erros / casos de borda
- **Lista vazia:** blob = `"Vazio"` + `"FIM"`. Master chama `commit()` então para.
- **Corrida 1º read:** load é síncrono na task I2C após o comando; master tolera read vazio.
- **Drain incompleto:** o master sempre lê até `"FIM"` (último registro) → FIFO sempre
  drenado ao fim do ciclo; o próximo `RequestSendFlag` recarrega em buffer limpo.
- **Underrun mid-registro** (improvável a 100 kHz com FIFO pré-preenchido): o master
  filtra `0x8f`; no pior caso um JSON sai inválido e o master loga e segue (lossy, não fatal).
- **MAC não encontrado no RX:** resposta de um MAC fora de `i2c_macs[]` é ignorada para o store.

## Testes

### A. Master real (ESP32/Arduino) — caminho de produção
Master envia `CLICKED` e roda `slaveRequest()`; verificar no log do slave
`I2C_SLAVE` os bytes carregados e, no master, os JSON parseados + `FIM`.

### B. Bancada sem o master (Raspberry Pi / adaptador USB-I2C)
1. Escrever o comando para armar a resposta:
   `i2cset -y <bus> 0x08 ...` (ou bloco "CLICKED").
2. Ler 72 bytes repetidamente e inspecionar (filtrando 0xFF):
   `i2ctransfer -y <bus> r72@0x08` várias vezes até aparecer `FIM`.
3. Esperado: 1º..N-ésimo read = JSONs por MAC; último = `FIM`; lista vazia = `Vazio` + `FIM`.

### C. Validação de alinhamento
Confirmar que cada read de 72 bytes contém exatamente um `}` (fim de um JSON) ou
`FIM`/`Vazio` — nenhum JSON partido entre reads.

## Fora de escopo
- Campo `tensao` real na mesh (hoje `0`; exige mudar `read_response_t` e os nós).
- Mudanças no master (fixo).
- Migração para `driver/i2c_slave.h`.
