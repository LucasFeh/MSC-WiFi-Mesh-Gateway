# Monitor de heap do ROOT via MQTT — telemetria ativável por botão

Data: 2026-06-29
Relacionado: `2026-06-03-ota-monitor-design.md`, `2026-06-18-i2c-to-mqtt-migration-design.md`

## Objetivo

Verificar se a heap livre do ROOT cai ao longo do tempo (suspeita de vazamento de memória),
sem precisar olhar o Serial. O ROOT publica a própria heap num tópico MQTT, ligado/desligado
por um **botão** no dashboard Flask. As amostras são gravadas num **CSV** no Raspberry para
plotagem offline (horas/dias) e mostradas ao vivo na UI.

Telemetria **desligada por padrão**; só publica quando o monitor está ligado, para não gerar
tráfego desnecessário quando ninguém está observando.

## Decisões (confirmadas pelo usuário 2026-06-29)

- **Métricas:** `free_heap` (= `esp_get_free_heap_size()`) + `min_free_heap`
  (= `esp_get_minimum_free_heap_size()`, o watermark mínimo histórico — só cai se houver leak)
  + `uptime_s` (pra estimar a taxa, bytes/hora).
- **Período:** fixo, 10 s (`#define HEAP_MONITOR_PERIOD_S 10`). O comando só liga/desliga,
  idêntico ao `mesh/cmd/otamon`. Não é configurável em runtime (YAGNI).
- **Ativação por botão:** OFF por padrão; botão no dashboard liga/desliga.
- **Comando retained:** o Flask é dono do estado e publica o comando **retained**. Se o ROOT
  reiniciar com o monitor ligado, ele volta ligado sozinho (a mensagem retida é reentregue na
  reinscrição em `mesh/cmd/#`). Bom para observar leak por horas mesmo com reboot.
- **Persistência:** CSV no Pi (`heap_log.csv`) + leitura numérica ao vivo na UI. Sem gráfico
  ao vivo no navegador (o CSV cobre a análise temporal).

## Protocolo de fio

### Flask → ROOT — comando (tópico `mesh/cmd/heapmon`, QoS 1, **retained**)
```json
{ "on": true }
```
`on` é detectado por substring `"true"` (mesmo estilo do `otamon` atual em `on_command`).

### ROOT → Flask — telemetria (tópico `mesh/root/heap`, QoS 0, não-retido)
```json
{ "free_heap": 123456, "min_free_heap": 120000, "uptime_s": 3600 }
```
- `free_heap` / `min_free_heap` em bytes; `uptime_s` em segundos (`esp_timer_get_time() / 1e6`).
- QoS 0 não-retido: telemetria de alta frequência, perder uma amostra é irrelevante (igual a
  `mesh/reading`).

## Ativação (botão) — fluxo

1. Seção "Monitor de heap" no dashboard tem um botão **Ativar/Desativar**.
2. Clicar emite `socket.emit('set_heap_monitor', {on})` → Flask atualiza `_heap_monitor_enabled`,
   publica `{"on":<bool>}` **retained** em `mesh/cmd/heapmon`, e reemite `heap_monitor_state`.
3. ROOT (`on_command`) reconhece `mesh/cmd/heapmon`, extrai `on` por substring e seta
   `heap_monitor_enabled`. Ao **ligar**, publica uma amostra na hora (feedback imediato).
4. A task `heap_timer` publica a heap a cada 10 s **somente** se `heap_monitor_enabled`.
   OFF ⇒ a task acorda, não publica e volta a dormir (custo desprezível).
5. **Re-sync:** em `MQTT_EVENT_CONNECTED` o ROOT reinscreve em `mesh/cmd/#` e recebe o comando
   retido; em `_on_connect` o Flask reasserta o estado atual de `_heap_monitor_enabled`.

## Estado

### Firmware
```c
volatile bool heap_monitor_enabled = false;   // mesh_main.c (extern em mesh_main.h)
```

### Flask
```python
_heap_monitor_enabled = False
_heap_latest = None     # último sample: {"free","min","uptime_s","ts"}
_heap_csv = ".../heap_log.csv"   # colunas: ts,free,min,uptime_s
```

## Mudanças por arquivo

### Firmware — `main/`
- `flask_request.h`:
  - `#define TOPIC_HEAP "mesh/root/heap"`
  - `extern void post_heap_to_flask(void);`
- `flask_request.c`:
  - `post_heap_to_flask(void)`: gateado por `s_mqtt_connected`; monta o JSON com `snprintf`
    (`free_heap`, `min_free_heap`, `uptime_s`) e publica em `TOPIC_HEAP` (QoS 0, não-retido).
    Inclui `esp_timer.h` (uptime) e o header das funções de heap (`esp_heap_caps.h`/`esp_system.h`).
  - `on_command`: novo ramo `mesh/cmd/heapmon` → `heap_monitor_enabled = (strstr(buf,"true")!=NULL)`;
    log `[MQTT] HEAPMON ON/OFF`; se ligou, chama `post_heap_to_flask()` (amostra imediata).
- `mesh_main.h`:
  - `extern volatile bool heap_monitor_enabled;`
  - `void heap_timer(void *arg);`
- `mesh_main.c`:
  - `volatile bool heap_monitor_enabled = false;` (junto dos `pending_*`).
  - `#define HEAP_MONITOR_PERIOD_S 10` + task `heap_timer` (espelha `status_timer`):
    `while(1){ if(heap_monitor_enabled) post_heap_to_flask(); vTaskDelay(10s); }`.
  - `xTaskCreate(heap_timer, "HEAP", 4096, NULL, 4, NULL);` junto da criação dos timers existentes.

### Flask — `Sidecars/server/app.py`
- Constantes `T_CMD_HEAPMON = "mesh/cmd/heapmon"`, `T_HEAP = "mesh/root/heap"`; caminho `_heap_csv`.
- Globais `_heap_monitor_enabled`, `_heap_latest` (sob `_lock`).
- `_on_connect`: `client.subscribe(T_HEAP, qos=0)` + reasserta `T_CMD_HEAPMON` retained
  (igual ao bloco do otamon).
- `_on_message`: ramo `T_HEAP` → `_apply_heap(payload)`: monta o sample (com `ts`), guarda em
  `_heap_latest`, anexa linha no CSV (cria cabeçalho se novo) e `socketio.emit('heap_update', sample)`.
- `@socketio.on('set_heap_monitor')`: atualiza flag, `_publish(T_CMD_HEAPMON, {"on":on}, retain=True)`,
  emite `heap_monitor_state`.
- `@app.get('/api/heap/state')`: retorna `{monitor_enabled, latest}` (load inicial).
- `@app.get('/api/heap/log.csv')`: `send_file(_heap_csv)` para baixar o histórico.

### Front-end — `Sidecars/server/`
- `templates/index.html`: nova `<section>` "Monitor de heap" (espelha `#otaMonitor`): botão
  `Ativar monitor de heap`, leitura ao vivo (free atual / mínimo / uptime), link "baixar CSV".
  Reusa as classes `.ota-monitor` / `.mon-toggle`.
- `static/script/script.js`:
  - `toggleHeapMonitor()` → `socket.emit('set_heap_monitor', {on:!heapMonitorEnabled})`.
  - `socket.on('heap_update', renderHeap)` (atualiza números), `socket.on('heap_monitor_state', ...)`
    (sincroniza botão); no load, `fetch('/api/heap/state')`.

## Não-objetivos / fora de escopo

- Gráfico/sparkline ao vivo no navegador (o CSV cobre a análise temporal).
- Métricas de fragmentação (maior bloco contíguo) e split interno/PSRAM.
- Período configurável em runtime (fixo em 10 s via `#define`).
- Mudanças no firmware do Driver/NODE (inalterado — só o ROOT publica a própria heap).
- Rotação/limite de tamanho do CSV (append simples; o usuário gerencia o arquivo).

## Verificação

- Flask: `python -m py_compile Sidecars/server/app.py`.
- Firmware: inspeção (sem ESP-IDF configurado nesta máquina para build/flash).
- Manual: ligar o monitor pelo botão → ver o número de free heap ao vivo na UI e o `heap_log.csv`
  crescendo; deixar rodando e plotar `free`/`min` vs `ts` pra confirmar (ou descartar) o leak.
