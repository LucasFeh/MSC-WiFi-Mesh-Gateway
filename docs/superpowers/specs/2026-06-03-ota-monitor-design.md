# Monitor OTA no Flask — telemetria do ROOT, ativável por botão

Data: 2026-06-03
Relacionado: `2026-06-02-mesh-ota-routing-design.md`, `2026-06-03-ota-unicast-target-design.md`

## Objetivo

Ver pelo dashboard Flask — sem olhar o Serial — o que o ROOT está fazendo no OTA:
para quem está enviando, a porcentagem de envio, e quem recebeu a atualização e quem não.
Cobre tanto o **self-update do ROOT** (arquivo `Gateway`) quanto a **distribuição via mesh**
(broadcast e unicast). Inclui **histórico** dos últimos envios.

A telemetria é **desligada por padrão** e ligada por um **botão** no dashboard, para não
sobrecarregar o gateway com envios desnecessários quando ninguém está olhando.

## Decisões (confirmadas pelo usuário 2026-06-03)

- **Transporte:** Abordagem A — HTTP POST do ROOT para `POST /api/ota/progress`, espelhando
  `post_status_to_flask`/`post_reading_to_flask`. O Flask reemite ao navegador via `socketio`.
- **Ao vivo + histórico:** mostra o OTA atual/último e mantém os últimos ~10 envios. Tudo em
  memória no Flask (igual a `_devices`); reseta se o Flask reiniciar.
- **Cobertura:** ROOT (self-update) + nós da mesh. A tabela "quem recebeu" só se aplica à mesh.
- **Ativação por botão:** telemetria OFF por padrão. Botão no dashboard liga/desliga.

## Ativação (botão) — fluxo

1. Seção "Monitor OTA" tem um botão **OFF/ON**.
2. Clicar emite `socket.emit('set_ota_monitor', {on})` → Flask atualiza `_ota_monitor_enabled`,
   empurra `{"cmd":"OTAMON","on":<bool>}` ao ROOT pelo WS, e reemite `ota_monitor_state` a todos.
3. ROOT (`ws_event_handler`) parseia `OTAMON` e chama `ota_set_monitor(on)` → flag `s_ota_monitor`.
4. `post_ota_event(...)` só envia se `s_ota_monitor` estiver ligado. OFF ⇒ zero POST extra.
5. **Re-sync:** ao (re)conectar o WS do ROOT (`_ws_root_handler`), o Flask reenvia o estado atual
   de `_ota_monitor_enabled` (cobre o ROOT ter reiniciado após self-update).

## Protocolo de fio

### Flask → ROOT (WebSocket, mesmo canal de `OTA`/`READ`)
```json
{ "cmd": "OTAMON", "on": true }
```

### ROOT → Flask (HTTP POST `/api/ota/progress`, body JSON com campo `event`)
- start (mesh):  `{ "event":"start", "op":"mesh"|"unicast", "file":"Driver-1.bin", "total":1843200, "targets":["aa:bb:..","cc:dd:.."] }`
- start (self):  `{ "event":"start", "op":"self", "file":"Gateway.bin", "total":524288 }`
- progress:      `{ "event":"progress", "pct":40 }`           (throttle: só quando o passo de 10% muda)
- ack (mesh):    `{ "event":"ack", "mac":"aa:bb:..", "status":"ok"|"fail" }`
- done (mesh):   `{ "event":"done", "targets":[{"mac":"aa:bb:..","status":"ok"|"fail"|"timeout"}, ...] }`
- done (self):   `{ "event":"done", "op":"self", "status":"rebooting" }`

`event` é detectado por substring (mesmo estilo do parse WS atual). MACs em minúsculas.

## Estado no Flask

```python
_ota_monitor_enabled = False
_ota_state = {
    "active": False, "op": None, "file": None, "total": 0, "pct": 0,
    "started": None, "finished": None,
    "targets": [],            # [{"mac":.., "status":"pending"|"ok"|"fail"|"timeout"}]
    "self_status": None,      # "running"|"rebooting" (op self)
}
_ota_history = []             # últimos 10: {"file","op","finished","ok","fail","timeout"}
```

Eventos atualizam `_ota_state`; `done` anexa um resumo a `_ota_history` (cap 10) e zera `active`.
Cada atualização dispara `socketio.emit('ota_progress', {state, history, monitor_enabled})`.

## Mudanças por arquivo

### Firmware — `main/`
- `flask_request.h`: declarar `void post_ota_event(const char *json_body);` e `void ota_set_monitor(bool on);`
  (esta última também pode ficar em `ota.h`; fica em `ota.h` por pertencer ao OTA).
- `flask_request.c`:
  - `post_ota_event(const char *body)`: POST JSON para `/api/ota/progress` (gateado por
    `is_got_ip && flask_connected`, como os outros posts). Não conhece o flag de monitor.
  - `ws_event_handler`: reconhecer `"OTAMON"`; extrair `on` (true/false por substring) e chamar
    `ota_set_monitor(...)`.
  - Definir `FLASK_OTA_PROGRESS_URL` junto das outras URLs do header.
- `ota.h`: declarar `void ota_set_monitor(bool on);`
- `ota.c`:
  - `static volatile bool s_ota_monitor = false;` + `ota_set_monitor()`.
  - Helpers internos (gateados por `s_ota_monitor`): `ota_report_start/progress/ack/done`,
    cada um monta o JSON com `snprintf` e chama `post_ota_event`.
  - `ota_self_update_task`: report start/progress/done("rebooting") antes do `esp_restart()`.
  - `ota_mesh_distribute_task`: report start (com lista de MACs) → progress no loop de chunks
    (throttle 10%) → no loop de espera de ACK, detectar transições e reportar `ack` por nó →
    `done` com o status final de cada nó. **Não** reportar de dentro de `ota_root_register_ack`
    (roda no contexto da task RX da mesh; um POST bloqueante ali atrasaria a recepção).

### Flask — `server/app.py`
- Globais `_ota_monitor_enabled`, `_ota_state`, `_ota_history` (+ usar `_lock`).
- `@socketio.on('set_ota_monitor')`: atualiza flag, empurra `OTAMON` ao ROOT, emite `ota_monitor_state`.
- `_ws_root_handler`: ao conectar, reenviar o estado atual de `_ota_monitor_enabled` ao ROOT.
- `@app.post('/api/ota/progress')`: aplica o evento em `_ota_state`/`_ota_history`, emite `ota_progress`.
- `@app.get('/api/ota/state')`: retorna `{monitor_enabled, state, history}` (load inicial).

### Front-end
- `server/templates/index.html`: nova `<section>` "Monitor OTA" (largura cheia, acima do `main`):
  botão toggle, linha de operação+arquivo+barra `%`, tabela de alvos (MAC · status), lista de histórico.
- `server/static/script/script.js`:
  - no load, `fetch('/api/ota/state')` para popular; `socket.on('ota_progress')` e
    `socket.on('ota_monitor_state')` para atualizar ao vivo.
  - botão → `socket.emit('set_ota_monitor', {on:!estadoAtual})`.
  - render: barra de progresso, ícones de status (pendente/✅/❌/⏱), histórico.
  - quando monitor OFF, seção em estado "desativado" (esmaecida) com aviso.
- `server/static/style/styles.css`: estilos da seção, barra de progresso, badges de status, toggle.

## Não-objetivos / fora de escopo

- Persistência do histórico em disco (mantém em memória).
- Mudanças no firmware do Driver/NODE e no pacote de fio `BIN_MSG_OTA` (inalterados).
- Monitor genérico de "tudo que o ROOT faz" além do OTA (YAGNI).

## Verificação

- `python -m py_compile server/app.py`.
- Firmware: inspeção (sem ESP-IDF configurado nesta máquina para build/flash).
- Manual: ligar o monitor, disparar um OTA broadcast/unicast e ver barra + tabela + histórico no Flask.
