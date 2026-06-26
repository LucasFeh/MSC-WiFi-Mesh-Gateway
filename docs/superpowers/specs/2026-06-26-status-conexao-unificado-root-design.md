# Status de conexão unificado (root tratado como qualquer nó)

**Data:** 2026-06-26
**Evolui:** `2026-06-23-status-polled-request-response-design.md` (mantém o modelo
de liveness por timeout; remove o tratamento especial do root e o cascade)
**Repos afetados:**
- `MSC-WiFi-Mesh-Gateway` (firmware do root) — 1 linha
- `Sidecars/server` (servidor Flask) — modelo de online

## Problema

1. **O root tem status de conexão próprio, separado do resto.** O Flask trata o
   nó `layer == 1` por um caminho diferente (`mesh/root/state`, retained + LWT) e
   ainda usa esse estado como porteiro de todos os outros
   (`if not root_online: return False`).
2. **Cascade derruba todo mundo offline antes do timeout per-nó.** O keepalive
   MQTT do root é 30 s; quando expira, o LWT publica `mesh/root/state =
   {"online":false}`, `_root_online` vira `False` e **todos** os nós caem offline
   na hora — muito antes do timeout de liveness de cada nó. É o "joga todo mundo
   pra offline antes do timeout".
3. **O root só publica o próprio status se tiver IP.** Em `mesh_main.c:282` o
   `post_status_to_flask` do root está atrás de `if (is_got_ip)`. O gate é
   redundante (o publish já aborta se o MQTT não estiver conectado, o que exige
   IP) e, junto com (1), faz o root depender de um caminho diferente do resto.
4. **Contexto — o `app.py` do Sidecars está com o modelo errado.** Ele usa
   `mesh/routing` + `mesh/online` para decidir online, mas o firmware atual nunca
   publica esses tópicos (só `mesh/status`, `mesh/offline`, `mesh/root/state`,
   `mesh/reading`, `mesh/ota/progress`). Com o firmware de hoje, nenhum nó que
   não seja o root chega a ficar online nesse arquivo.

## Decisões (travadas)

- **Uma regra de liveness só, igual para todos (root incluso):**
  > online ⟺ recebeu `mesh/status`/`mesh/reading` nos últimos
  > `DEVICE_OFFLINE_TIMEOUT_S = 35` s **e** sem `mesh/offline` desde então.
- **Timeout = 35 s** (~3,5 polls de status de 10 s). Tolera ~3 polls perdidos
  sem falso-offline; reflete queda real em ≤ 35 s.
- **Sem tratamento especial do root no Flask:** apagar o ramo `layer == 1 →
  root_online`, a variável `_root_online`, o cascade `if not root_online:
  return False` e o handler/assinatura de `T_ROOT_STATE`.
- **Root publica o próprio status a cada poll:** remover o gate `if (is_got_ip)`
  em `mesh_main.c:282`. Toda vez que o `status_timer` (10 s) roda, o root publica
  o seu `mesh/status` (layer 1) e, com a regra acima, fica online sozinho.
- **`mesh/root/state` fica inofensivo:** o firmware **continua** publicando o
  retained + LWT (sem mudança); o Flask simplesmente **não assina nem usa** mais.
- **Trocar o modelo do `Sidecars/server/app.py`** do routing-table para o de
  timeout/last_seen (que casa com o firmware), já sem o caminho especial do root.
- **UI intocada:** o pill "Root" e a árvore já derivam de `devices[].online`
  (`script.js:38` acha `layer === 1 && online`); passam a refletir a regra única
  automaticamente.

## Arquitetura nova (Flask)

```
mesh/status  (cada nó + o próprio root, a cada 10 s) ─► _apply_status:
                                                          online=True, last_seen=now
mesh/offline (CHILD_DISCONNECTED de filho direto)    ─► _devices[mac].online=False
mesh/reading                                         ─► (telemetria; UI)

_device_online(info) =  bool(info.online) AND _is_fresh(info.last_seen, 35 s)
   — mesmo cálculo para TODOS, inclusive layer 1.
   — sem _root_online, sem _online_set, sem cascade.

_watch_root (loop 1 s) reavalia e emite state_update quando algo expira.
```

## Mudanças por componente

### A. Firmware — `MSC-WiFi-Mesh-Gateway/main/mesh_main.c`

1. Linha ~282: remover o `if (is_got_ip) { ... }` que envolve o
   `post_status_to_flask` do próprio root. O corpo (ler rssi/mac, montar e
   publicar o status layer 1) passa a rodar sempre que `pending_status_broadcast`
   dispara. Nada mais muda no firmware (`mesh/root/state`/LWT ficam).

### B. Flask — `Sidecars/server/app.py`

1. **Tópicos:** remover as constantes `T_ONLINE = "mesh/online"`,
   `T_ROUTING = "mesh/routing"` e `T_ROOT_STATE = "mesh/root/state"` (não são mais
   assinadas nem tratadas).
2. **Estado:** remover `_online_set` e `_root_online`.
3. **Constante + helper:** adicionar `DEVICE_OFFLINE_TIMEOUT_S = 35` e
   `_is_fresh(last_seen_iso)` (parse ISO; `True` se `now - ts ≤ timeout`;
   `False` em parse inválido) — espelhando o `server/app.py` do gateway.
4. **`_device_online(info)`** (assinatura sem `mac`/`root_online`):
   `return bool(info.get("online", False)) and _is_fresh(info.get("last_seen", ""))`.
   Sem ramo de `layer == 1`.
5. **`_get_state`:** chamar `_device_online(info)` sem `root_online`.
6. **`_on_connect`:** assinar só `T_READING, T_STATUS, T_OFFLINE, T_OTA_PROGRESS`
   (tirar `T_ONLINE`, `T_ROUTING`, `T_ROOT_STATE`).
7. **`_on_message`:**
   - `T_OFFLINE`: `if mac in _devices: _devices[mac]["online"] = False` (em vez de
     `_online_set.discard`).
   - remover os ramos `T_ONLINE`, `T_ROUTING` e `T_ROOT_STATE` (e o
     `global _root_online`).
8. **`_apply_status`:** intocado (já grava `online=True` e `last_seen=now`).
9. **`_watch_root`:** ajustar a chamada para `_device_online(info)` (sem
   `root_online`); mantém o loop de 1 s.

### C. UI

Nenhuma mudança.

## Comportamento resultante

- Root e nós usam exatamente o mesmo cálculo de online/offline.
- O root aparece online enquanto publicar status (a cada 10 s); cai offline só
  após 35 s de silêncio — igual a qualquer nó.
- A queda/keepalive do root **não** derruba mais a malha inteira de uma vez: cada
  nó expira pelo próprio `last_seen`. (Se o root morre de fato, ele para de
  encaminhar status e todos expiram naturalmente em ≤ 35 s.)
- `mesh/offline` continua derrubando um filho na hora no disconnect direto.

## Não-objetivos (YAGNI)

- Não mexer em leitura de canais, OTA, nem no período de poll (10 s) do firmware.
- Não remover o `mesh/root/state`/LWT do firmware (decisão: deixar inofensivo).
- Não sincronizar com o `MSC-WiFi-Mesh-Gateway/server/app.py` (cópia paralela);
  o alvo é só o `Sidecars/server/app.py`.
- Não trocar o `BROKER_HOST` do Sidecars (`192.168.15.191`) — fora de escopo.

## Verificação

- **Flask (pytest):**
  - `_is_fresh`: agora-mesmo → `True`; `last_seen` de 40 s atrás → `False`;
    string inválida → `False`.
  - `_device_online`: nó fresco com `online=True` → online; mesmo nó com
    `last_seen` expirado → offline; `online=False` (pós `mesh/offline`) → offline.
  - **Root (layer 1)** com status fresco → online; root sem status há > 35 s →
    offline (prova que não há mais caminho especial).
  - Cenário do bug: vários nós frescos + root expira → os nós **continuam**
    online (não há cascade).
- **Firmware:** build limpo; `mosquitto_sub -t mesh/status -v` mostra o status do
  root (layer 1) a cada ~10 s mesmo logo após boot/reconexão.
