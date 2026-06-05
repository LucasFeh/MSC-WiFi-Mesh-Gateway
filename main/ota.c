#include "ota.h"
#include <string.h>
#include <stdio.h>           /* snprintf */
#include <stdlib.h>
#include <stddef.h>          /* offsetof */
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"        /* esp_wifi_get_mac, WIFI_IF_STA */
#include "esp_mac.h"         /* MACSTR / MAC2STR              */
#include "esp_rom_crc.h"     /* esp_rom_crc32_le              */

/* OTA_HDR_SIZE vem de ota_protocol.h (compartilhado com o Driver). */
/* -------------------------------------------------------------------------- */
/* Barra de progresso reutilizável nos dois fluxos.
 *   [OTA] self [████████░░░░░░░░░░░░] 40% (204800 / 512000 bytes)
 * 20 blocos: U+2588 (cheio, "\xE2\x96\x88") e U+2591 (vazio, "\xE2\x96\x91"). */
static void ota_print_progress(const char *tag, size_t written, size_t total)
{
    if (total == 0) return;
    int pct = (int)((uint64_t)written * 100 / (uint64_t)total);
    int filled = pct / 5;                 /* 20 blocos -> 1 bloco = 5% */
    if (filled > 20) filled = 20;

    char bar[20 * 3 + 1];                  /* cada bloco UTF-8 ocupa 3 bytes */
    int p = 0;
    for (int i = 0; i < 20; i++) {
        const char *blk = (i < filled) ? "\xE2\x96\x88" : "\xE2\x96\x91";
        bar[p++] = blk[0]; bar[p++] = blk[1]; bar[p++] = blk[2];
    }
    bar[p] = '\0';

    ESP_LOGI(MESH_TAG, "[OTA] %s [%s] %d%% (%u / %u bytes)",
             tag, bar, pct, (unsigned)written, (unsigned)total);
}

/* -------------------------------------------------------------------------- */
/* Estado da distribuição via mesh (Fluxo B) + coleta de ACK dos nós.         */
typedef struct {
    uint8_t  mac[6];
    bool     acked;             /* concluiu o OTA (END confirmado OK)             */
    uint8_t  status;            /* OTA_ACK_OK / OTA_ACK_FAIL                       */
    uint32_t expected_offset;   /* último offset que o nó pediu (stop-and-wait)   */
    bool     have_ack;          /* recebeu ACK desde o último ponto de sincronia  */
    bool     failed;            /* reportou FAIL ou esgotou as retransmissões     */
} ota_target_t;

static ota_target_t      s_targets[CONFIG_MESH_ROUTE_TABLE_SIZE];
static int               s_target_count = 0;
static volatile bool     s_dist_active  = false;
static SemaphoreHandle_t s_dist_mutex   = NULL;

/* Stop-and-wait ARQ: cada pacote OTA é confirmado pelo nó (campo expected_offset
 * do ota_ack_t) antes de o root avançar. Sem ACK em OTA_ACK_TIMEOUT_MS o root
 * retransmite o MESMO pacote; após OTA_MAX_RETRIES tentativas o nó é marcado
 * 'failed' e a distribuição segue com os demais. */
#define OTA_ACK_TIMEOUT_MS   2000
#define OTA_MAX_RETRIES      5

/* Argumento da task de distribuição via mesh. Quando 'unicast' é true, o alvo é
 * 'mac' (um único nó); caso contrário a lista de alvos vem da routing table.
 * 'name' é o nome do .bin, repassado ao NODE no BEGIN p/ o filtro de variante. */
typedef struct {
    char    url[160];
    char    name[OTA_FW_NAME_MAX];
    bool    unicast;
    uint8_t mac[6];
} ota_dist_arg_t;

/* Argumento da task de self-update (Fluxo A): URL + nome do .bin (este último
 * só para a telemetria do monitor). */
typedef struct {
    char url[160];
    char name[OTA_FW_NAME_MAX];
} ota_self_arg_t;

/* ===========================================================================
 *  Telemetria OTA para o dashboard (Monitor OTA).
 *  Desligada por padrão; ligada pelo comando WS "OTAMON" -> ota_set_monitor().
 *  Com s_ota_monitor=false NENHUM POST é emitido (zero overhead no gateway).
 *  Os helpers montam o JSON e chamam post_ota_event() (HTTP POST -> Flask).
 * ========================================================================== */
static volatile bool s_ota_monitor = false;

void ota_set_monitor(bool on)
{
    s_ota_monitor = on;
}

static void ota_report_start_self(const char *name, size_t total)
{
    if (!s_ota_monitor) return;
    char body[160];
    snprintf(body, sizeof(body),
             "{\"event\":\"start\",\"op\":\"self\",\"file\":\"%s\",\"total\":%u}",
             name, (unsigned)total);
    post_ota_event(body);
}

/* start da distribuição mesh: inclui a lista de MACs alvo (lida de s_targets,
 * já populada pela task). Limita os MACs ao que couber no buffer. */
static void ota_report_start_mesh(bool unicast, const char *name, size_t total)
{
    if (!s_ota_monitor) return;
    char body[512];
    int p = snprintf(body, sizeof(body),
        "{\"event\":\"start\",\"op\":\"%s\",\"file\":\"%s\",\"total\":%u,\"targets\":[",
        unicast ? "unicast" : "mesh", name, (unsigned)total);
    for (int i = 0; i < s_target_count && p > 0 && p < (int)sizeof(body) - 24; i++) {
        p += snprintf(body + p, sizeof(body) - (size_t)p, "%s\""MACSTR"\"",
                      i ? "," : "", MAC2STR(s_targets[i].mac));
    }
    snprintf(body + p, sizeof(body) - (size_t)p, "]}");
    post_ota_event(body);
}

static void ota_report_progress(int pct)
{
    if (!s_ota_monitor) return;
    char body[48];
    snprintf(body, sizeof(body), "{\"event\":\"progress\",\"pct\":%d}", pct);
    post_ota_event(body);
}

static void ota_report_ack(const uint8_t mac[6], uint8_t status)
{
    if (!s_ota_monitor) return;
    char body[96];
    snprintf(body, sizeof(body),
             "{\"event\":\"ack\",\"mac\":\""MACSTR"\",\"status\":\"%s\"}",
             MAC2STR(mac), status == OTA_ACK_OK ? "ok" : "fail");
    post_ota_event(body);
}

/* done da mesh: o Flask deriva o resumo (alvos ainda 'pendentes' viram timeout). */
static void ota_report_done_mesh(void)
{
    if (!s_ota_monitor) return;
    post_ota_event("{\"event\":\"done\"}");
}

static void ota_report_done_self(const char *status)
{
    if (!s_ota_monitor) return;
    char body[96];
    snprintf(body, sizeof(body),
             "{\"event\":\"done\",\"op\":\"self\",\"status\":\"%s\"}", status);
    post_ota_event(body);
}

/* Chamado pelo RX da mesh (mesh_main.c) a cada OTA_ACK de um nó. Atualiza o
 * progresso do stop-and-wait: 'expected_offset' é o próximo offset que o nó quer
 * receber. status=FAIL marca o nó como perdido (será pulado no resto do envio). */
void ota_root_register_ack(const uint8_t from_mac[6], uint8_t status, uint32_t expected_offset)
{
    if (!s_dist_mutex || !s_dist_active) return;
    xSemaphoreTake(s_dist_mutex, portMAX_DELAY);
    for (int i = 0; i < s_target_count; i++) {
        if (memcmp(s_targets[i].mac, from_mac, 6) == 0) {
            s_targets[i].status          = status;
            s_targets[i].expected_offset = expected_offset;
            s_targets[i].have_ack        = true;
            if (status == OTA_ACK_FAIL) s_targets[i].failed = true;
            break;
        }
    }
    xSemaphoreGive(s_dist_mutex);
    ESP_LOGI(MESH_TAG, "[OTA] ACK de "MACSTR" status=%u next=%u",
             MAC2STR(from_mac), status, (unsigned)expected_offset);
}

/* Stop-and-wait ARQ. Envia 'pkt' (size bytes) aos alvos ainda ativos que não
 * confirmaram 'want_offset' e aguarda o OTA_ACK de cada um (expected_offset >=
 * want_offset). Retransmite os atrasados a cada OTA_ACK_TIMEOUT_MS; após
 * OTA_MAX_RETRIES tentativas marca o nó como 'failed' e segue com os demais.
 * Retorna o nº de nós ainda ativos (não 'failed'); 0 = todos perdidos.
 * O esp_mesh_send roda FORA do mutex p/ não travar o RX (ota_root_register_ack).
 * esp_mesh_send: docs ESP-WIFI-MESH (ver ota_protocol.h). */
static int ota_send_chunk_sync(const ota_packet_t *pkt, size_t size, uint32_t want_offset)
{
    mesh_data_t d = {
        .data  = (uint8_t *)pkt,
        .size  = (uint16_t)size,
        .proto = MESH_PROTO_BIN,
        .tos   = MESH_TOS_P2P,
    };

    /* Só contam ACKs deste want_offset: zera have_ack dos alvos ativos. */
    xSemaphoreTake(s_dist_mutex, portMAX_DELAY);
    for (int i = 0; i < s_target_count; i++)
        if (!s_targets[i].failed) s_targets[i].have_ack = false;
    xSemaphoreGive(s_dist_mutex);

    for (int attempt = 0; attempt < OTA_MAX_RETRIES; attempt++) {
        /* Snapshot (sob mutex) dos alvos que ainda precisam receber o pacote. */
        mesh_addr_t pend[CONFIG_MESH_ROUTE_TABLE_SIZE];
        int npend = 0, active = 0;
        xSemaphoreTake(s_dist_mutex, portMAX_DELAY);
        for (int i = 0; i < s_target_count; i++) {
            if (s_targets[i].failed) continue;
            active++;
            if (s_targets[i].have_ack && s_targets[i].expected_offset >= want_offset) continue;
            memcpy(pend[npend++].addr, s_targets[i].mac, 6);
        }
        xSemaphoreGive(s_dist_mutex);

        if (active == 0) return 0;          /* todos os nós já falharam          */
        if (npend  == 0) return active;     /* todos confirmaram este want_offset */

        for (int k = 0; k < npend; k++) {
            esp_err_t e = esp_mesh_send(&pend[k], &d, MESH_DATA_P2P, NULL, 0);
            if (e != ESP_OK)
                ESP_LOGW(MESH_TAG, "[OTA] send -> "MACSTR" err 0x%x (tent %d/%d @off %u)",
                         MAC2STR(pend[k].addr), e, attempt + 1, OTA_MAX_RETRIES,
                         (unsigned)want_offset);
        }

        /* Aguarda os ACKs chegarem (ota_root_register_ack atualiza s_targets). */
        TickType_t start = xTaskGetTickCount();
        while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(OTA_ACK_TIMEOUT_MS)) {
            bool all_ok = true; int act = 0;
            xSemaphoreTake(s_dist_mutex, portMAX_DELAY);
            for (int i = 0; i < s_target_count; i++) {
                if (s_targets[i].failed) continue;
                act++;
                if (!(s_targets[i].have_ack && s_targets[i].expected_offset >= want_offset))
                    all_ok = false;
            }
            xSemaphoreGive(s_dist_mutex);
            if (act == 0)  return 0;
            if (all_ok)    return act;
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (attempt + 1 < OTA_MAX_RETRIES)
            ESP_LOGW(MESH_TAG, "[OTA] timeout @off %u — retransmitindo (tent %d/%d)",
                     (unsigned)want_offset, attempt + 1, OTA_MAX_RETRIES);
    }

    /* Tentativas esgotadas: marca 'failed' quem não confirmou want_offset. */
    int active = 0;
    xSemaphoreTake(s_dist_mutex, portMAX_DELAY);
    for (int i = 0; i < s_target_count; i++) {
        if (s_targets[i].failed) continue;
        if (s_targets[i].have_ack && s_targets[i].expected_offset >= want_offset) {
            active++;
        } else {
            s_targets[i].failed = true;
            s_targets[i].status = OTA_ACK_FAIL;
            ESP_LOGE(MESH_TAG, "[OTA] no "MACSTR" perdido @off %u (%d tentativas) — pulando",
                     MAC2STR(s_targets[i].mac), (unsigned)want_offset, OTA_MAX_RETRIES);
        }
    }
    xSemaphoreGive(s_dist_mutex);
    return active;
}

/* ========================================================================== */
/* Fluxo A: nome contém "Gateway" -> ROOT atualiza a si mesmo.                */
static void ota_self_update_task(void *arg)
{
    ota_self_arg_t *a = (ota_self_arg_t *)arg;
    const char *url = a->url;
    bool started = false;          /* true após emitir o evento 'start' ao monitor */
    ESP_LOGI(MESH_TAG, "[OTA] self: download de %s", url);

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) { ESP_LOGE(MESH_TAG, "[OTA] sem partição OTA"); goto done; }

    esp_http_client_config_t hcfg = { .url = url, .timeout_ms = 15000, .keep_alive_enable = true };
    esp_http_client_handle_t http = esp_http_client_init(&hcfg);
    if (!http) { ESP_LOGE(MESH_TAG, "[OTA] http init falhou"); goto done; }
    if (esp_http_client_open(http, 0) != ESP_OK) {
        ESP_LOGE(MESH_TAG, "[OTA] http open falhou");
        esp_http_client_cleanup(http); goto done;
    }
    int64_t total = esp_http_client_fetch_headers(http);
    if (total <= 0) {
        ESP_LOGE(MESH_TAG, "[OTA] Content-Length inválido (%lld)", total);
        esp_http_client_close(http); esp_http_client_cleanup(http); goto done;
    }
    ESP_LOGI(MESH_TAG, "[OTA] firmware: %lld bytes", total);
    ota_report_start_self(a->name, (size_t)total);
    started = true;

    esp_ota_handle_t h;
    if (esp_ota_begin(part, (size_t)total, &h) != ESP_OK) {
        ESP_LOGE(MESH_TAG, "[OTA] ota_begin falhou");
        esp_http_client_close(http); esp_http_client_cleanup(http); goto done;
    }

    uint8_t *buf = malloc(OTA_CHUNK_MAX);
    if (!buf) { esp_ota_abort(h); esp_http_client_close(http); esp_http_client_cleanup(http); goto done; }

    size_t written = 0; int last_pct = -1; int last_rep = -1; bool err = false; int r;
    while ((r = esp_http_client_read(http, (char *)buf, OTA_CHUNK_MAX)) > 0) {
        if (esp_ota_write(h, buf, r) != ESP_OK) { ESP_LOGE(MESH_TAG, "[OTA] write falhou"); err = true; break; }
        written += r;
        int pct = (int)((uint64_t)written * 100 / (uint64_t)total);
        if (pct != last_pct) { ota_print_progress("self", written, total); last_pct = pct; }
        if (pct / 10 != last_rep / 10) { last_rep = pct; ota_report_progress(pct); }  /* throttle ~10% */
    }
    free(buf);
    esp_http_client_close(http);
    esp_http_client_cleanup(http);

    if (err || written == 0) { esp_ota_abort(h); ESP_LOGE(MESH_TAG, "[OTA] self abortado"); goto done; }
    if (esp_ota_end(h) != ESP_OK)               { ESP_LOGE(MESH_TAG, "[OTA] ota_end falhou");  goto done; }
    if (esp_ota_set_boot_partition(part) != ESP_OK) { ESP_LOGE(MESH_TAG, "[OTA] set_boot falhou"); goto done; }

    ESP_LOGI(MESH_TAG, "[OTA] self completo! reiniciando...");
    ota_report_done_self("rebooting");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

done:
    if (started) ota_report_done_self("fail");   /* chegou aqui sem reiniciar = falhou */
    free(a);
    vTaskDelete(NULL);
}

/* ========================================================================== */
/* Fluxo B: nome contém "Driver" -> ROOT repassa via mesh, em fluxo (stream), */
/* sem aplicar OTA em si mesmo e sem reiniciar até confirmar entrega (ACK).   */
static void ota_mesh_distribute_task(void *arg)
{
    ota_dist_arg_t *a = (ota_dist_arg_t *)arg;
    const char *url = a->url;
    ESP_LOGI(MESH_TAG, "[OTA] mesh: download de %s", url);

    if (s_dist_active) { ESP_LOGW(MESH_TAG, "[OTA] distribuição já em andamento, abortando"); goto done; }

    /* 1) Monta a lista de alvos.
     *    - unicast: um único MAC, escolhido no dashboard.
     *    - broadcast: todos os nós da routing table, excluindo o próprio root. */
    xSemaphoreTake(s_dist_mutex, portMAX_DELAY);
    s_target_count = 0;
    if (a->unicast) {
        memcpy(s_targets[0].mac, a->mac, 6);
        s_targets[0].acked           = false;
        s_targets[0].status          = OTA_ACK_FAIL;
        s_targets[0].expected_offset = 0;
        s_targets[0].have_ack        = false;
        s_targets[0].failed          = false;
        s_target_count = 1;
    } else {
        mesh_addr_t route[CONFIG_MESH_ROUTE_TABLE_SIZE];
        int route_size = 0;
        if (esp_mesh_get_routing_table(route, sizeof(route), &route_size) != ESP_OK) {
            ESP_LOGE(MESH_TAG, "[OTA] esp_mesh_get_routing_table falhou");
            xSemaphoreGive(s_dist_mutex); goto done;
        }
        uint8_t self_mac[6];
        esp_wifi_get_mac(WIFI_IF_STA, self_mac);
        for (int i = 0; i < route_size && s_target_count < CONFIG_MESH_ROUTE_TABLE_SIZE; i++) {
            if (memcmp(route[i].addr, self_mac, 6) == 0) continue;   /* não envia para si */
            memcpy(s_targets[s_target_count].mac, route[i].addr, 6);
            s_targets[s_target_count].acked           = false;
            s_targets[s_target_count].status          = OTA_ACK_FAIL;
            s_targets[s_target_count].expected_offset = 0;
            s_targets[s_target_count].have_ack        = false;
            s_targets[s_target_count].failed          = false;
            s_target_count++;
        }
    }
    int N = s_target_count;
    s_dist_active = true;
    xSemaphoreGive(s_dist_mutex);

    if (a->unicast)
        ESP_LOGI(MESH_TAG, "[OTA] unicast -> "MACSTR, MAC2STR(a->mac));
    ESP_LOGI(MESH_TAG, "[OTA] Nos encontrados: %d", N);
    if (N == 0) { ESP_LOGW(MESH_TAG, "[OTA] nenhum nó na mesh, nada a distribuir"); goto done_active; }

    /* 2) Download em fluxo do Flask. */
    esp_http_client_config_t hcfg = { .url = url, .timeout_ms = 15000, .keep_alive_enable = true };
    esp_http_client_handle_t http = esp_http_client_init(&hcfg);
    if (!http) { ESP_LOGE(MESH_TAG, "[OTA] http init falhou"); goto done_active; }
    if (esp_http_client_open(http, 0) != ESP_OK) {
        ESP_LOGE(MESH_TAG, "[OTA] http open falhou"); esp_http_client_cleanup(http); goto done_active;
    }
    int64_t total = esp_http_client_fetch_headers(http);
    if (total <= 0) {
        ESP_LOGE(MESH_TAG, "[OTA] Content-Length inválido (%lld)", total);
        esp_http_client_close(http); esp_http_client_cleanup(http); goto done_active;
    }
    ESP_LOGI(MESH_TAG, "[OTA] firmware: %lld bytes -> %d nó(s)", total, N);
    ota_report_start_mesh(a->unicast, a->name, (size_t)total);   /* alvos já em s_targets */

    ota_packet_t *pkt = malloc(sizeof(*pkt));
    if (!pkt) { esp_http_client_close(http); esp_http_client_cleanup(http); goto done_active; }

    /* 2a) BEGIN (cabeçalho + nome do .bin em payload, p/ o NODE filtrar a variante). */
    pkt->msg_id = BIN_MSG_OTA; pkt->tipo = OTA_BEGIN;
    pkt->total = (uint32_t)total; pkt->offset = 0; pkt->crc = 0;
    size_t namelen = strlen(a->name);           /* a->name é null-terminated (snprintf) */
    if (namelen > OTA_FW_NAME_MAX - 1) namelen = OTA_FW_NAME_MAX - 1;
    memcpy(pkt->payload, a->name, namelen);
    pkt->payload[namelen] = '\0';
    pkt->chunk_size = (uint32_t)(namelen + 1);   /* inclui o terminador */
    if (ota_send_chunk_sync(pkt, OTA_HDR_SIZE + namelen + 1, 0) == 0) {
        ESP_LOGE(MESH_TAG, "[OTA] nenhum nó confirmou o BEGIN — abortando");
        free(pkt); esp_http_client_close(http); esp_http_client_cleanup(http);
        goto done_active;
    }

    /* 2b) CHUNKs em fluxo. */
    size_t sent = 0; uint32_t offset = 0; int last_pct = -1; int last_rep = -1; int r;
    while ((r = esp_http_client_read(http, (char *)pkt->payload, OTA_CHUNK_MAX)) > 0) {
        pkt->msg_id = BIN_MSG_OTA; pkt->tipo = OTA_CHUNK;
        pkt->total = (uint32_t)total; pkt->offset = offset; pkt->chunk_size = (uint32_t)r;
        pkt->crc = esp_rom_crc32_le(0, pkt->payload, r);   /* [SEM FONTE: esp_rom_crc32_le] — CRC32 do ROM do ESP-IDF (esp_rom/include/esp_rom_crc.h) */
        uint32_t want = offset + (uint32_t)r;
        if (ota_send_chunk_sync(pkt, OTA_HDR_SIZE + (size_t)r, want) == 0) {
            ESP_LOGE(MESH_TAG, "[OTA] todos os nós perdidos @off %u — interrompendo envio",
                     (unsigned)offset);
            break;
        }
        offset = want; sent += (size_t)r;
        int pct = (int)((uint64_t)sent * 100 / (uint64_t)total);
        if (pct != last_pct) { ota_print_progress("mesh", sent, total); last_pct = pct; }
        if (pct / 10 != last_rep / 10) { last_rep = pct; ota_report_progress(pct); }  /* throttle ~10% */
    }

    /* 2c) END (só cabeçalho). */
    pkt->msg_id = BIN_MSG_OTA; pkt->tipo = OTA_END;
    pkt->total = (uint32_t)total; pkt->offset = offset; pkt->chunk_size = 0; pkt->crc = 0;
    ota_send_chunk_sync(pkt, OTA_HDR_SIZE, (uint32_t)total);

    free(pkt);
    esp_http_client_close(http);
    esp_http_client_cleanup(http);

    /* 3) O stop-and-wait já confirmou cada pacote inline. Consolida o status
     *    final: quem chegou ao 'total' sem falhar concluiu (OK); o resto falhou.
     *    ROOT NÃO reinicia. */
    int ok = 0;
    uint8_t rmac[CONFIG_MESH_ROUTE_TABLE_SIZE][6];
    uint8_t rst[CONFIG_MESH_ROUTE_TABLE_SIZE];
    int rn = 0;
    xSemaphoreTake(s_dist_mutex, portMAX_DELAY);
    for (int i = 0; i < s_target_count; i++) {
        bool done = !s_targets[i].failed && s_targets[i].expected_offset >= (uint32_t)total;
        s_targets[i].acked  = done;
        s_targets[i].status = done ? OTA_ACK_OK : OTA_ACK_FAIL;
        if (done) ok++;
        memcpy(rmac[rn], s_targets[i].mac, 6);
        rst[rn] = s_targets[i].status;
        rn++;
    }
    xSemaphoreGive(s_dist_mutex);

    /* Telemetria e log fora do mutex (o POST HTTP não pode bloquear o RX). */
    for (int i = 0; i < rn; i++) {
        if (rst[i] == OTA_ACK_OK)
            ESP_LOGI(MESH_TAG, "[OTA] nó "MACSTR": OK", MAC2STR(rmac[i]));
        else
            ESP_LOGW(MESH_TAG, "[OTA] nó "MACSTR": FALHOU/timeout", MAC2STR(rmac[i]));
        ota_report_ack(rmac[i], rst[i]);
    }
    ESP_LOGI(MESH_TAG, "[OTA] distribuição concluída: %d/%d confirmado(s)", ok, N);
    ota_report_done_mesh();   /* Flask marca alvos ainda 'pendentes' como timeout */

done_active:
    s_dist_active = false;
done:
    free(a);
    vTaskDelete(NULL);
}

/* Aloca o argumento da task de distribuição e a cria. Retorna false se falhar. */
static bool ota_start_distribute(const char *url, const char *name, bool unicast, const uint8_t *mac)
{
    ota_dist_arg_t *a = calloc(1, sizeof(*a));
    if (!a) { ESP_LOGE(MESH_TAG, "[OTA] sem memória p/ distribuição"); return false; }
    snprintf(a->url, sizeof(a->url), "%s", url);
    snprintf(a->name, sizeof(a->name), "%s", name ? name : "");
    a->unicast = unicast;
    if (unicast && mac) memcpy(a->mac, mac, 6);
    if (xTaskCreate(ota_mesh_distribute_task, "ota_mesh", 8192, a, 5, NULL) != pdPASS) {
        ESP_LOGE(MESH_TAG, "[OTA] xTaskCreate(ota_mesh) falhou");
        free(a);
        return false;
    }
    return true;
}

/* ========================================================================== */
/* Ponto de entrada: unicast (target_mac != NULL) tem prioridade; senão roteia
 * pelo NOME e dispara a task correspondente. */
void trigger_ota(const char *url, const char *name, const uint8_t *target_mac)
{
    if (!url || !name) { ESP_LOGE(MESH_TAG, "[OTA] url/nome nulo"); return; }
    if (!s_dist_mutex) s_dist_mutex = xSemaphoreCreateMutex();

    /* UNICAST: envia o firmware só para o MAC escolhido, ignorando o nome. */
    if (target_mac) {
        ESP_LOGI(MESH_TAG, "[OTA] Arquivo recebido: %s", name);
        ESP_LOGI(MESH_TAG, "[OTA] Destino: unicast "MACSTR, MAC2STR(target_mac));
        ota_start_distribute(url, name, true, target_mac);
        return;
    }

    /* LOG [OTA] com a decisão ANTES de qualquer operação OTA. */
    if (strstr(name, "Gateway")) {
        ESP_LOGI(MESH_TAG, "[OTA] Arquivo recebido: %s", name);
        ESP_LOGI(MESH_TAG, "[OTA] Destino: ROOT (self-update)");
        ota_self_arg_t *sa = calloc(1, sizeof(*sa));
        if (sa) {
            snprintf(sa->url,  sizeof(sa->url),  "%s", url);
            snprintf(sa->name, sizeof(sa->name), "%s", name);
            if (xTaskCreate(ota_self_update_task, "ota_self", 8192, sa, 5, NULL) != pdPASS) {
                ESP_LOGE(MESH_TAG, "[OTA] xTaskCreate(ota_self) falhou");
                free(sa);
            }
        }
    } else if (strstr(name, "Driver") || strstr(name, "Driver-1") || strstr(name, "Driver-3")) {
        ESP_LOGI(MESH_TAG, "[OTA] Arquivo recebido: %s", name);
        ESP_LOGI(MESH_TAG, "[OTA] Destino: nos via Mesh");
        ota_start_distribute(url, name, false, NULL);
    } else {
        ESP_LOGW(MESH_TAG, "[OTA] nome desconhecido: %s (esperado Gateway*/Driver*), ignorando", name);
    }
}