# Prompt: OTA ESP-WIFI-MESH — ROOT com roteamento por nome de arquivo

## Premissas arquiteturais fixas

> "esp_mesh_send() can only be used for internal data communication in the Wi-Fi Mesh network.
> To send data from a leaf node to an external server, the data needs to be forwarded through the root node."
> — [ESP-FAQ: Wi-Fi Mesh](https://docs.espressif.com/projects/esp-faq/en/latest/application-solution/wifi-mesh-development-framework.html)

> "The OTA approach is that the root node downloads the firmware and then disseminates it to other nodes."
> — mesma fonte

> "Do not transmit an entire firmware file: ESP-WIFI-MESH is a multi-hop network, which means it can
> only guarantee a reliable transmission from node to node, and NOT end to end."
> — [ESP-MDF Mupgrade Guide](https://docs.espressif.com/projects/esp-mdf/en/latest/api-guides/mupgrade.html)

**Consequência inegociável:** nós não-root não possuem acesso IP externo.
Toda OTA de nó obrigatoriamente transita pelo root via `esp_mesh_send()`.

---

## Contexto do projeto

| Variável         | Valor a preencher                                        |
|------------------|----------------------------------------------------------|
| `ROOT_PATH`      | caminho absoluto para `root/`                            |
| `NODE_PATH`      | caminho absoluto para `node/`                            |
| `FLASK_URL`      | URL base do servidor Flask (ex: `http://192.168.1.100:5000`) |
| `ESP_IDF_VERSION`| versão do ESP-IDF em uso                                 |

---

## Fase 1 — `@superpowers:reading-code` — Leitura completa do código

**Pergunta central:** O que o código atual faz no ROOT ao receber um `.bin` via Flask,
e como está o tratamento OTA e Mesh hoje?

Leia e transcreva — **sem modificar, sem diagnosticar** — os seguintes trechos:

### ROOT (`ROOT_PATH`):
- Endpoint Flask (ou handler HTTP): como o `.bin` é recebido? Qual campo ou header identifica o nome do arquivo?
- Existe extração ou verificação do nome do arquivo recebido?
- Sequência completa do fluxo OTA atual: quais APIs são chamadas e em qual ordem?
- Após `esp_ota_end()`: qual é a próxima chamada?
- Existe `esp_mesh_send()` para distribuição de firmware? Com quais flags e chunk size?
- Existe `esp_mesh_get_routing_table()`? Em que posição relativa ao download?
- Existe `esp_restart()`? Em que posição relativa ao envio mesh?

### NODE (`NODE_PATH`):
- Existe task ou handler que processa mensagens mesh relacionadas a OTA?
- Existe chamada a `esp_ota_begin()`, `esp_ota_write()`, `esp_ota_end()`?
- Existe `esp_https_ota()` ou `esp_http_client_*`?

### Partition tables (ambos os projetos):
- Transcreva o conteúdo de `partitions.csv` de cada projeto
- Confirme presença de: `otadata`, `ota_0`, `ota_1`

### Flask (`app.py` ou equivalente):
- Liste todos os endpoints existentes
- Como o nome do arquivo `.bin` é exposto ao ROOT (header HTTP, campo multipart, nome do endpoint, etc.)?

**Gate Fase 1:** Você deve ter transcrito, para ROOT **e** NODE: fluxo OTA completo, presença
ou ausência de distribuição mesh, partition tables, e como o nome do `.bin` chega ao ROOT.
Item ausente = leia o arquivo correspondente **antes de avançar**.

---

## Fase 2 — `@superpowers:systematic-debugging` — Diagnóstico estruturado

**Pergunta central:** Quais causas explicam o ROOT atualizar a si mesmo sem repassar firmware para os nós?

Preencha com evidência de `arquivo:linha` para cada causa:

| #  | Causa hipotética                                                                          | Evidência (arquivo:linha) | Status |
|----|-------------------------------------------------------------------------------------------|---------------------------|--------|
|  1 | ROOT não extrai o nome do `.bin` — sem como distinguir `"Gateway"` de `"Driver-*"`       |                           | ✅/❌  |
|  2 | ROOT aplica OTA em si mesmo independente do nome do arquivo                               |                           | ✅/❌  |
|  3 | ROOT chama `esp_restart()` antes de distribuir firmware aos nós                           |                           | ✅/❌  |
|  4 | ROOT não implementa `esp_mesh_send()` para distribuição de firmware                       |                           | ✅/❌  |
|  5 | Node não tem task OTA (nenhum handler de mensagem mesh para OTA)                          |                           | ✅/❌  |
|  6 | Chunk size > 1400 bytes em `esp_mesh_send()` (se existir)                                 |                           | ✅/❌  |
|  7 | Partition table do node sem `ota_0` / `ota_1`                                             |                           | ✅/❌  |
|  8 | `MESH_TOS_P2P` ausente nos flags de `esp_mesh_send()` (se existir)                       |                           | ✅/❌  |
|  9 | ROOT não chama `esp_mesh_get_routing_table()` antes de distribuir                         |                           | ✅/❌  |

**Gate Fase 2:** Toda causa deve ter status ✅ ou ❌ com referência a `arquivo:linha`.
Status indefinido = Fase 2 incompleta. **Não avance com células em branco.**

---

## Fase 3 — `@superpowers:writing-plans` — Protocolo correto

**Pergunta central:** Qual é o fluxo OTA correto para esta topologia, conforme documentação?
**Descreva em linguagem natural — sem código.**

### 3.1 — ROOT: recepção e decisão de roteamento

1. ROOT recebe o arquivo `.bin` via Flask
2. ROOT extrai o nome do arquivo recebido
3. ROOT imprime no Serial com tag `[OTA]` **antes de qualquer operação**:

   - Se nome contém `"Gateway"`:
     ```
     [OTA] Arquivo recebido: Gateway.bin
     [OTA] Destino: ROOT (self-update)
     ```
   - Se nome contém `"Driver-1"` ou `"Driver-3"`:
     ```
     [OTA] Arquivo recebido: Driver-1.bin
     [OTA] Destino: nos via Mesh
     ```

4. ROOT toma a decisão de roteamento com base no nome

### 3.2 — Fluxo A: nome contém `"Gateway"` → Self-update do ROOT

1. ROOT processa o `.bin` já recebido do Flask
2. ROOT aplica OTA em si mesmo via `esp_ota_begin()` → `esp_ota_write()` → `esp_ota_end()`
3. Durante a escrita, ROOT exibe barra de progresso no Serial:
   ```
   [OTA] Progresso: [████████░░░░░░░░░░░░] 40% (204800 / 512000 bytes)
   ```
   - Barra de 20 blocos: `█` preenchido, `░` vazio
   - Atualizada a cada chunk processado
4. Após `esp_ota_end()`: `esp_ota_set_boot_partition()` → `esp_restart()`

### 3.3 — Fluxo B: nome contém `"Driver-1"` ou `"Driver-3"` → Repasse via Mesh

1. ROOT armazena o `.bin` em buffer RAM (sem aplicar OTA em si mesmo)
2. ROOT chama `esp_mesh_get_routing_table()` para obter MACs dos nós conectados
3. ROOT imprime no Serial: `[OTA] Nos encontrados: N`
4. ROOT fragmenta o binário em chunks de no máximo 1400 bytes
5. Para cada nó, para cada chunk: ROOT chama `esp_mesh_send()` com flag `MESH_TOS_P2P`
6. ROOT exibe progresso do envio no Serial:
   ```
   [OTA] Enviando para no 1/N (AA:BB:CC:DD:EE:FF): [████████░░░░░░░░░░░░] 40%
   ```
7. ROOT aguarda confirmação (ACK) de cada nó antes de prosseguir
8. ROOT **não reinicia** até confirmar entrega a todos os nós alvo

### 3.4 — NODE: recepção e aplicação de OTA

1. Node mantém task bloqueada em `esp_mesh_recv()`
2. Ao receber chunk OTA: chama `esp_ota_write()` na partição obtida via
   `esp_ota_get_next_update_partition()`
3. Node exibe progresso no Serial com tag `[OTA]` a cada chunk recebido
4. Ao receber chunk final: `esp_ota_end()` → `esp_ota_set_boot_partition()` → `esp_restart()`

### 3.5 — Struct de mensagem OTA (campos necessários, sem código)

```
tipo        (OTA_BEGIN / OTA_CHUNK / OTA_END)
total       tamanho total do firmware em bytes
offset      posição do chunk atual no firmware
chunk_size  tamanho deste chunk
payload     dados do chunk
crc         CRC do chunk para validação
```

**Gate Fase 3:** O protocolo deve cobrir: extração do nome, decisão de roteamento,
log serial `[OTA]`, barra de progresso, Fluxo A (self), Fluxo B (mesh), recepção
no node, struct de mensagem. **Nenhum trecho de código nesta fase.**

---

## Fase 4 — `@superpowers:brainstorming` — Gap analysis

**Pergunta central:** O que falta ou está errado entre o estado atual (Fase 1) e o protocolo correto (Fase 3)?

| Componente                                              | Estado atual (Fase 1) | Estado necessário (Fase 3) | Gap |
|---------------------------------------------------------|-----------------------|----------------------------|-----|
| ROOT: extração do nome do `.bin` recebido               |                       |                            |     |
| ROOT: decisão de roteamento por nome                    |                       |                            |     |
| ROOT: log serial `[OTA]` com decisão antes da ação      |                       |                            |     |
| ROOT: barra de progresso `[█░]` no Serial               |                       |                            |     |
| ROOT: self-update apenas quando nome = `"Gateway"`      |                       |                            |     |
| ROOT: buffer do `.bin` para repasse sem self-update     |                       |                            |     |
| ROOT: `esp_mesh_get_routing_table()` antes de distribuir|                       |                            |     |
| ROOT: `esp_mesh_send()` com chunks ≤ 1400 bytes         |                       |                            |     |
| ROOT: aguarda ACK antes de `esp_restart()`              |                       |                            |     |
| NODE: task `esp_mesh_recv()` para OTA                   |                       |                            |     |
| NODE: `esp_ota_begin()` / `esp_ota_write()` / `esp_ota_end()` |                |                            |     |
| NODE: partition table com `ota_0` / `ota_1`             |                       |                            |     |
| Flask: endpoint que expõe o nome do arquivo ao ROOT     |                       |                            |     |

**Liste apenas os gaps confirmados** — a Fase 5 implementa somente estes.

**Gate Fase 4:** Cada linha deve estar preenchida.
**Não implemente nada antes de completar esta tabela.**

---

## Fase 5 — `@feature-dev:feature-dev` — Implementação

**Pergunta central:** Como implementar exatamente os gaps da Fase 4, usando apenas APIs documentadas?

### Restrições inegociáveis

| | Restrição |
|---|---|
| ❌ PROIBIDO | `esp_https_ota()` em qualquer código de node |
| ❌ PROIBIDO | Transmitir o `.bin` inteiro em um único `esp_mesh_send()` |
| ❌ PROIBIDO | `esp_restart()` no ROOT antes de confirmar entrega a todos os nós (Fluxo B) |
| ❌ PROIBIDO | Inventar APIs, funções ou comportamentos não encontrados no código atual |
| ❌ PROIBIDO | Refatorar código fora do escopo dos gaps identificados na Fase 4 |
| ✅ OBRIGATÓRIO | Chunk size: máximo 1400 bytes — definido como constante nomeada |
| ✅ OBRIGATÓRIO | Log `[OTA]` no Serial antes de qualquer operação OTA |
| ✅ OBRIGATÓRIO | Barra de progresso `[████░░░░] XX% (Y / Z bytes)` atualizada a cada chunk |
| ✅ OBRIGATÓRIO | Sinalizar `[SEM FONTE: nome_api]` para qualquer API fora da tabela abaixo |

### APIs autorizadas

| API | Uso | Documentação |
|-----|-----|--------------|
| `esp_mesh_send()` | ROOT→NODE: envio de chunks | https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_mesh.html |
| `esp_mesh_recv()` | NODE: recepção de chunks | mesma fonte |
| `esp_mesh_get_routing_table()` | ROOT: lista de MACs conectados | mesma fonte |
| `esp_ota_begin()` | Inicializa escrita na partição OTA | https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/ota.html |
| `esp_ota_write()` | Escreve chunk na partição | mesma fonte |
| `esp_ota_end()` | Finaliza e valida a imagem | mesma fonte |
| `esp_ota_set_boot_partition()` | Marca partição para boot | mesma fonte |
| `esp_ota_get_next_update_partition()` | Obtém partição OTA disponível | mesma fonte |
| `esp_http_client_*` | ROOT: download do Flask (se necessário) | https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/protocols/esp_http_client.html |
| `esp_restart()` | Reinicialização após OTA | https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/misc_system_api.html |

### Formato de entrega obrigatório

- **ROOT (`ROOT_PATH`):** diffs das funções afetadas — não reescrever o arquivo inteiro
- **NODE (`NODE_PATH`):** nova task OTA via mesh (função completa)
- **Struct de mensagem OTA:** definição com todos os campos comentados
- **Progresso Serial:** função `ota_print_progress(size_t written, size_t total)` separada e reutilizável nos dois fluxos
- **Flask:** apenas os endpoints ou alterações estritamente necessárias

Implemente **somente** os gaps identificados na Fase 4.

**Gate Fase 5:** Cada função implementada deve referenciar a API usada com link.
API fora da tabela = `[SEM FONTE]` obrigatório antes de usar.

---

## Fase 6 — `@code-review` (nível `high`) + `@superpowers:verification-before-completion` — Verificação final

**Pergunta central:** A solução está completa, correta e verificável?

| Item | Como verificar | Status |
|------|----------------|--------|
| Log `[OTA]` aparece **antes** de qualquer operação OTA | Confirme no código gerado | ✅/❌ |
| Decisão de roteamento baseada no nome do `.bin` | Confirme condicionais para `"Gateway"`, `"Driver-1"`, `"Driver-3"` | ✅/❌ |
| ROOT faz self-update **apenas** quando nome contém `"Gateway"` | Confirme no código gerado | ✅/❌ |
| ROOT repassa via Mesh **apenas** quando nome contém `"Driver-1"` ou `"Driver-3"` | Confirme no código gerado | ✅/❌ |
| ROOT **NÃO** reinicia antes de distribuir a todos os nós (Fluxo B) | Confirme no código gerado | ✅/❌ |
| NODE **NÃO** usa `esp_https_ota()` ou TCP externo | Confirme ausência | ✅/❌ |
| Chunk size ≤ 1400 bytes com constante definida | Confirme `#define` ou `constexpr` | ✅/❌ |
| `esp_mesh_get_routing_table()` presente no ROOT (Fluxo B) | Confirme chamada | ✅/❌ |
| NODE tem task `esp_mesh_recv()` para OTA | Confirme implementação | ✅/❌ |
| Barra de progresso `[█░] XX%` no Serial | Confirme formato e atualização por chunk | ✅/❌ |
| Ambas as partition tables têm `ota_0` / `ota_1` | Confirme via leitura da Fase 1 | ✅/❌ |
| Struct OTA tem: `tipo`, `total`, `offset`, `chunk_size`, `payload`, `crc` | Confirme campos | ✅/❌ |
| Toda API tem fonte na seção de Referências | Zero `[SEM FONTE]` não resolvidos | ✅/❌ |
| Nenhuma funcionalidade existente foi quebrada | Confirme via diff de arquivos não listados nos gaps | ✅/❌ |

**Se qualquer item estiver ❌, corrija a fase correspondente antes de encerrar.
Declare conclusão apenas com todos os itens ✅.**

---

## Referências

| Fonte | URL |
|-------|-----|
| ESP-WIFI-MESH API Reference | https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_mesh.html |
| ESP OTA API Reference | https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/ota.html |
| ESP HTTP Client API | https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/protocols/esp_http_client.html |
| ESP Misc System API (`esp_restart`) | https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/misc_system_api.html |
| ESP-FAQ: Wi-Fi Mesh OTA | https://docs.espressif.com/projects/esp-faq/en/latest/application-solution/wifi-mesh-development-framework.html |
| ESP-MDF Mupgrade Guide | https://docs.espressif.com/projects/esp-mdf/en/latest/api-guides/mupgrade.html |
| Chunk size: referência comunitária | https://www.esp32.com/viewtopic.php?t=15606 |
