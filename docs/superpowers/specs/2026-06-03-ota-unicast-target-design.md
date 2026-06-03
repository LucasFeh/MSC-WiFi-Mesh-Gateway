# OTA Unicast — envio direcionado a um MAC

Data: 2026-06-03
Relacionado: `2026-06-02-mesh-ota-routing-design.md`

## Objetivo

Permitir enviar um firmware `.bin` para **um único nó** da mesh, escolhido por MAC,
em vez do broadcast atual (que roteia por nome do arquivo: `Gateway`→self, `Driver`→toda a mesh).

## Fluxo de UX (dashboard Flask)

1. Na linha "Firmware OTA" há um checkbox **"Enviar unicast"**.
2. Ao **marcar** o checkbox, abre um **modal** que carrega a lista de dispositivos de
   `GET /api/state` (a lista que o Flask já mantém), **excluindo o ROOT** (`layer === 1`).
3. O usuário **seleciona um MAC**; o modal memoriza o alvo, mostra-o ao lado do botão e fecha.
4. O usuário clica no botão principal **"Enviar OTA"**; o upload segue com o alvo unicast.
5. Desmarcar o checkbox limpa o alvo e volta ao comportamento broadcast atual.

Se o checkbox estiver marcado sem MAC selecionado, o envio é bloqueado com aviso.

## Decisões (confirmadas pelo usuário 2026-06-03)

- **Fonte da lista:** `GET /api/state` (Flask já agrega via `/api/status`). Sem firmware novo só para listar.
- **Roteamento:** unicast **sobrepõe** o roteamento por nome — envia o `.bin` exatamente para o
  MAC escolhido, qualquer que seja o nome do arquivo.
- **Lista exclui o ROOT** (`layer === 1`): o ROOT só se atualiza via nome `Gateway`.
- **Disparo:** seleção no modal apenas memoriza+fecha; o envio dispara no botão "Enviar OTA" principal.
- **Abordagem (firmware):** estende o comando OTA existente com campo opcional `target` (Abordagem 1).
  Reaproveita o caminho de streaming + ACK já validado; nenhum comando WS novo.

## Protocolo de fio (WebSocket Flask → ROOT)

Mesmo canal/JSON do OTA atual, com um campo **opcional**:

```json
{ "cmd": "OTA", "file": "Driver-1.bin", "url": "http://.../firmware/latest.bin", "target": "aa:bb:cc:dd:ee:ff" }
```

- `target` ausente → comportamento atual (roteamento por nome).
- `target` presente → unicast para esse MAC, ignorando o roteamento por nome.

## Mudanças por arquivo

### Flask — `server/app.py`
- `POST /api/ota/upload`: ler campo opcional de form `target` (string MAC). Se presente e não vazio,
  incluir `"target"` no JSON empurrado ao ROOT pelo WS. Resposta inclui `target` para o front exibir.

### Front-end
- `server/templates/index.html`: checkbox "Enviar unicast" na `kpi-ota`; markup do modal (overlay +
  lista de dispositivos + botão fechar); span mostrando o alvo selecionado.
- `server/static/script/script.js`:
  - estado `unicastTarget`.
  - `change` do checkbox: marcado → abre modal e busca `/api/state`, renderiza devices com `layer !== 1`
    (marca offline visualmente, mas permite seleção); desmarcado → limpa `unicastTarget` e o rótulo.
  - clique numa linha → `unicastTarget = mac`, atualiza rótulo, fecha modal. Fechar sem selecionar
    desmarca o checkbox.
  - `sendOta()`: se checkbox marcado e sem alvo → aviso e aborta; se alvo definido → `formData.append("target", unicastTarget)`
    e mensagem de status reflete "unicast → <mac>".
- `server/static/style/styles.css`: estilos do modal/overlay e do checkbox/rótulo.

### Firmware — `main/`
- `ota.h`: assinatura `void trigger_ota(const char *url, const char *name, const uint8_t *target_mac);`
  (`target_mac` = `NULL` → roteamento por nome; não-nulo → unicast).
- `flask_request.c` (`ws_event_handler`): após extrair `file`/`url`, extrair `target` opcional;
  se presente, converter "aa:bb:cc:dd:ee:ff" → `uint8_t[6]` e passar o ponteiro a `trigger_ota`; senão `NULL`.
- `ota.c`:
  - `ota_mesh_distribute_task` passa a receber um struct de argumento
    `{ char url[160]; bool unicast; uint8_t mac[6]; }` (alocado no heap, liberado pela task).
    Quando `unicast`, pula a varredura de `esp_mesh_get_routing_table()` e define
    `s_targets[0] = mac; s_target_count = 1;`. Caso contrário, mantém a varredura atual.
  - `trigger_ota`: se `target_mac != NULL` → loga `[OTA] Destino: unicast MACSTR`, monta o struct com
    `unicast=true` e cria a task (ignora nome). Senão, roteamento atual: `Gateway`→`ota_self_update_task`,
    `Driver`→`ota_mesh_distribute_task` com `unicast=false`.

## Não-objetivos / fora de escopo

- Lista ao vivo da routing table do ROOT (descartado: usa a lista do Flask).
- Seleção múltipla de MACs (apenas 1 alvo por envio).
- Mudanças no firmware do Driver/NODE (o pacote de fio é o mesmo `BIN_MSG_OTA`).

## Verificação

- Inspeção + build do firmware (sem flashing em sessão, conforme limitação registrada).
- Front/Flask: teste manual do fluxo de modal e do `target` no payload WS.
