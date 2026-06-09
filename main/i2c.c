#include "i2c.h"
#include "driver/i2c.h"        /* API legada (não usar driver/i2c_slave.h) */
#include "esp_log.h"
#include "esp_system.h"        /* esp_restart() */
#include "nvs.h"               /* preferences -> NVS */
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>             /* sscanf */
#include <stdlib.h>            /* atoi */

static const char *TAG = "I2C_SLAVE";

/* Sinalizado toda vez que um novo MAC é adicionado via I2C;
   permite que a task de resposta pare de esperar e envie a lista. */
static SemaphoreHandle_t mac_added_sem = NULL;

/* Definições dos símbolos declarados como extern em i2c.h.
   São consumidos pela task TX da mesh (mesh_main.c); mantidos aqui
   para preservar o link, mesmo que esta versão não popule a lista. */
uint8_t           i2c_macs[MAX_MACS][6];
volatile int      i2c_mac_count = 0;
SemaphoreHandle_t i2c_macs_mutex = NULL;

/* Leituras por-MAC consumidas pelo onRequest (slave -> master). Populadas no RX
   da mesh; ver i2c.h. */
i2c_reading_t     i2c_readings[MAX_MACS] = {0};

/* --- Control surface: flags/estado setados pelos comandos recebidos via I2C.
   Devem ser consumidos pela lógica da mesh/aplicação (ver i2c.h). --- */
volatile bool RequestSendFlag      = false;
volatile bool flagCommit           = false;
volatile bool flagUnCommit         = false;
volatile bool flagUniscastUpdate   = false;
volatile bool flagUpdate           = false;
volatile bool flagReboot           = false;
volatile bool AP_FLAG              = false;
volatile bool timerAdjust          = false;
volatile bool flag_received        = false; /* para debug: sinaliza que chegou algo (mesmo que não tenhamos logado o conteúdo) */
volatile int  seconds              = 0;
char          numero[8]            = {0};   /* nº de série do comando UPDATE */
char          updateUnicastMacStr[18] = {0};

void i2c_slave_init(void)
{
    ESP_LOGI(TAG, "Inicializando I2C slave (API legada driver/i2c.h)...");

    i2c_macs_mutex = xSemaphoreCreateMutex();
    mac_added_sem    = xSemaphoreCreateBinary();

    i2c_config_t conf = {
        .mode                = I2C_MODE_SLAVE,
        .sda_io_num          = I2C_SDA_PIN,
        .scl_io_num          = I2C_SCL_PIN,
        .sda_pullup_en       = GPIO_PULLUP_ENABLE,   /* pull-ups internos habilitados */
        .scl_pullup_en       = GPIO_PULLUP_ENABLE,
        .slave.addr_10bit_en = 0,                    /* endereçamento de 7 bits */
        .slave.slave_addr    = I2C_SLAVE_ADDR,
        .clk_flags           = 0,
    };

    ESP_ERROR_CHECK(i2c_param_config(I2C_SLAVE_PORT, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_SLAVE_PORT, conf.mode,
                                       I2C_RX_BUF_SIZE, I2C_TX_BUF_SIZE, 0));

    ESP_LOGI(TAG, "I2C slave pronto: addr=0x%02X  SDA=%d  SCL=%d  RXbuf=%d  timeout=%dms",
             I2C_SLAVE_ADDR, I2C_SDA_PIN, I2C_SCL_PIN,
             I2C_RX_BUF_SIZE, I2C_READ_TIMEOUT_MS);
}

/* "AA:BB:CC:DD:EE:FF" -> 6 bytes. Retorna true em sucesso. */
static bool parseMAC(const char *str, uint8_t out[6])
{
    int v[6];
    if (sscanf(str, "%x:%x:%x:%x:%x:%x",
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        if (v[i] < 0 || v[i] > 0xFF) return false;
        out[i] = (uint8_t)v[i];
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * Dispatcher de comandos recebidos via I2C — porte da função onReceive() do
 * firmware Arduino. A leitura do barramento é feita por i2c_slave_task(); aqui
 * recebemos a string (null-terminada) já montada.
 * ------------------------------------------------------------------------- */
static void i2c_on_receive(const char *buffer)
{
    if (strcmp(buffer, "CLICKED") == 0) {
        pending_read_broadcast = true;
        return;
    }

    if (strcmp(buffer, "CLEAR") == 0) {
        return;
    }

    if (strcmp(buffer, "COMMIT") == 0) {
        pending_read_broadcast = true;
        return;
    }

    if (strcmp(buffer, "UNCOMMIT") == 0) {
        RequestSendFlag = true;
        flagUnCommit = true;
        return;
    }

    if (strcmp(buffer, "REBOOT") == 0) {
        pending_reboot = true;
        return;
    }

    if (strcmp(buffer, "RBOT_I2C") == 0) {
        esp_restart();
        return;
    }

    /* Ajuste do timer via "TIME:xx" */
    if (strncmp(buffer, "TIME:", 5) == 0) {
        seconds = atoi(buffer + 5);
        if (seconds > 0) {
            timerAdjust = true;
            ESP_LOGI(TAG, "Request de ajuste do timer recebido: %d segundos", seconds);
        } else {
            ESP_LOGI(TAG, "Valor de tempo invalido");
        }
        return;
    }

    /* Caso contrário: interpreta como um MAC a adicionar à lista da mesh. */
    uint8_t newMac[6];
    if (!parseMAC(buffer, newMac)) {
        ESP_LOGI(TAG, "MAC invalido");
        return;
    }

    if (i2c_macs_mutex) xSemaphoreTake(i2c_macs_mutex, portMAX_DELAY);

    for (int i = 0; i < i2c_mac_count; i++) {
        if (memcmp(newMac, i2c_macs[i], 6) == 0) {
            ESP_LOGI(TAG, "MAC ja existe");
            if (i2c_macs_mutex) xSemaphoreGive(i2c_macs_mutex);
            return;
        }
    }

    if (i2c_mac_count < MAX_MACS) {
        memcpy(i2c_macs[i2c_mac_count], newMac, 6);
        /* Slot de leitura zerado até a primeira resposta da mesh (porte do
           SensorData[macCount] do firmware Arduino). */
        memset(&i2c_readings[i2c_mac_count], 0, sizeof(i2c_readings[0]));
        ESP_LOGI(TAG, "MAC %d armazenado: %s", i2c_mac_count, buffer);
        i2c_mac_count++;
        /* Avisa a task de resposta que chegou pelo menos um MAC. */
        if (mac_added_sem) xSemaphoreGive(mac_added_sem);
    } else {
        ESP_LOGI(TAG, "Limite de MACs atingido");
    }

    if (i2c_macs_mutex) xSemaphoreGive(i2c_macs_mutex);
}

/* Os 17 chars a partir de 's' formam um MAC "HH:HH:HH:HH:HH:HH"? */
static bool i2c_is_mac_token(const char *s)
{
    for (int i = 0; i < 17; i++) {
        char c = s[i];
        if (c == '\0') return false;
        if (i == 2 || i == 5 || i == 8 || i == 11 || i == 14) {
            if (c != ':') return false;            /* separadores nas posições fixas */
        } else if (!isxdigit((unsigned char)c)) {
            return false;                          /* demais posições: dígito hex */
        }
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * Framing RX: o driver legado NÃO preserva fronteiras de STOP do I2C — ele
 * concatena no ring buffer tudo que chegou dentro da janela de leitura. O master
 * envia mensagens coladas, sem delimitador. Como os MACs têm formato fixo de 17
 * chars e os comandos são palavras-chave, fatiamos o buffer aqui e despachamos
 * cada mensagem isoladamente para i2c_on_receive() (que espera uma por vez).
 * ------------------------------------------------------------------------- */
static void i2c_split_and_dispatch(const char *buffer)
{
    static const char *kw[] = { "CLICKED", "UNCOMMIT", "COMMIT", "CLEAR", "RBOT_I2C" };
    const char *p = buffer;
    char token[32];

    while (*p) {
        bool matched = false;

        /* 1) Palavra-chave conhecida (comandos são alfabéticos; não colidem com MAC). */
        for (size_t i = 0; i < sizeof(kw) / sizeof(kw[0]); i++) {
            size_t klen = strlen(kw[i]);
            if (strncmp(p, kw[i], klen) == 0) {
                i2c_on_receive(kw[i]);
                p += klen;
                matched = true;
                break;
            }
        }
        if (matched) continue;

        /* 2) "TIME:<dígitos>" */
        if (strncmp(p, "TIME:", 5) == 0) {
            const char *q = p + 5;
            while (isdigit((unsigned char)*q)) q++;
            size_t tlen = (size_t)(q - p);
            if (tlen < sizeof(token)) {
                memcpy(token, p, tlen);
                token[tlen] = '\0';
                i2c_on_receive(token);
            }
            p = q;
            continue;
        }

        /* 3) MAC de 17 chars. */
        if (i2c_is_mac_token(p)) {
            memcpy(token, p, 17);
            token[17] = '\0';
            i2c_on_receive(token);
            p += 17;
            continue;
        }

        /* 4) Byte não reconhecido: avança 1 (defensivo, evita laço infinito). */
        p++;
    }
}

static size_t i2c_emit_record(uint8_t *blob, size_t off, const char *text)
{
    size_t tlen = strlen(text);
    if (tlen > I2C_REQUEST_STRIDE) tlen = I2C_REQUEST_STRIDE;   /* trava de segurança */
    memcpy(blob + off, text, tlen);
    memset(blob + off + tlen, 0xFF, I2C_REQUEST_STRIDE - tlen);
    return off + I2C_REQUEST_STRIDE;
}

/* Serializa a lista atual de MACs+leituras no blob (um registro por MAC, depois
   "FIM"; lista vazia -> "Vazio" + "FIM"). Retorna o tamanho total escrito. */
static size_t i2c_build_request_blob(uint8_t *blob, size_t cap)
{
    size_t off = 0;
    char   rec[I2C_REQUEST_STRIDE + 1];

    if (i2c_macs_mutex) xSemaphoreTake(i2c_macs_mutex, portMAX_DELAY);

    int count = i2c_mac_count;
    if (count <= 0) {
        off = i2c_emit_record(blob, off, "Vazio");
        ESP_LOGI(TAG, "Nenhum MAC cadastrado: resposta com 'Vazio'");
    } else {
        for (int i = 0; i < count && off + I2C_REQUEST_STRIDE <= cap; i++) {
            // ESP_LOGI(TAG, "Serializando leitura para MAC %d: %02x:%02x:%02x:%02x:%02x:%02x",
            //          i, i2c_macs[i][0], i2c_macs[i][1], i2c_macs[i][2],
            //             i2c_macs[i][3], i2c_macs[i][4], i2c_macs[i][5]);

            uint8_t ch1 = i2c_readings[i].ch1;
            uint8_t ch2 = i2c_readings[i].ch2;
            uint8_t ch3 = i2c_readings[i].ch3;
            uint8_t tensao = i2c_readings[i].tensao;

            /* Sem resposta há >=3 ciclos de broadcast: sensor offline -> zeros
               (porte do "if (rTCounter == 3)" do onRequest Arduino). */
            if (i2c_readings[i].rTCounter >= 3) {
                ch1 = ch2 = ch3 = tensao = 0;
            }

            snprintf(rec, sizeof(rec),
                     "{\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\","
                     "\"ch1\":%u,\"ch2\":%u,\"ch3\":%u,\"tensao\":%u}",
                     i2c_macs[i][0], i2c_macs[i][1], i2c_macs[i][2],
                     i2c_macs[i][3], i2c_macs[i][4], i2c_macs[i][5],
                     (unsigned)ch1, (unsigned)ch2, (unsigned)ch3, (unsigned)tensao);
            off = i2c_emit_record(blob, off, rec);
        }
    }

    if (i2c_macs_mutex) xSemaphoreGive(i2c_macs_mutex);

    /* Sentinela final: o master encerra o loop de leitura ao receber "FIM". */
    if (off + I2C_REQUEST_STRIDE <= cap) {
        off = i2c_emit_record(blob, off, "FIM");
    }
    return off;
}

/* Task dedicada do caminho slave -> master. Monta o blob atual (MACs+leituras +
   "FIM") e o entrega ao ring buffer TX UM REGISTRO POR VEZ, bloqueando por-registro
   quando o buffer está cheio. Esse backpressure fino: (a) não tem busy-wait,
   (b) limita o atraso a ~I2C_TX_DEPTH_RECORDS leituras — evitando o acúmulo de
   blobs velhos na fila (causa do "Vazio" repetido), (c) escreve sempre múltiplos
   de I2C_REQUEST_STRIDE, preservando o alinhamento do FIFO. */
void i2c_slave_request_task(void *arg)
{
    static uint8_t blob[I2C_REQUEST_BLOB_MAX];

    ESP_LOGI(TAG, "Task de resposta I2C iniciada");

    while (1) {

        size_t len = i2c_build_request_blob(blob, sizeof(blob));

        /* Envia cada registro ao FIFO TX (portMAX_DELAY bloqueia se cheio). */
        for (size_t off = 0; off + I2C_REQUEST_STRIDE <= len; off += I2C_REQUEST_STRIDE) {
            int w = i2c_slave_write_buffer(I2C_SLAVE_PORT, blob + off,
                                           I2C_REQUEST_STRIDE, portMAX_DELAY);
            if (w != I2C_REQUEST_STRIDE) {
                ESP_LOGE(TAG, "Escrita TX incompleta (%d)", w);
                break;
            }
        }

        if (i2c_mac_count == 0) {
            /* Lista vazia: "Vazio" foi enviado UMA vez.
               Aguarda até 5 s pelo master empurrar MACs via I2C write.
               - Se chegarem MACs: xSemaphoreGive acorda esta task imediatamente.
               - Se o timeout estourar: reenvia "Vazio" e espera de novo. */
            ESP_LOGI(TAG, "Aguardando MACs do master (timeout 5 s)...");
            xSemaphoreTake(mac_added_sem, pdMS_TO_TICKS(5000));
        }
        /* Se count > 0: volta ao topo imediatamente para servir o próximo blob. */
    }
}

void i2c_slave_task(void *arg)
{
    uint8_t rx_data[I2C_RX_BUF_SIZE];
    char    printable[I2C_RX_BUF_SIZE + 1];   /* +1 para terminação nula */

    ESP_LOGI(TAG, "Task de leitura I2C iniciada (timeout %d ms)", I2C_READ_TIMEOUT_MS);

    while (1) {
        int len = i2c_slave_read_buffer(I2C_SLAVE_PORT, rx_data, sizeof(rx_data),
                                        pdMS_TO_TICKS(I2C_READ_TIMEOUT_MS));

        if (len < 0) {
            ESP_LOGE(TAG, "Erro ao ler buffer I2C (%d)", len);
            continue;
        }
        if (len == 0) {
            /* Nenhum dado dentro do timeout: volta a aguardar. */
            continue;
        }

        i2c_on_receive(""); /* sinaliza que chegou algo, mesmo que não loguemos o conteúdo (ex: leitura parcial >0 mas <len) */

        /* ---- Dados válidos recebidos (len > 0) ---- */
        ESP_LOGI(TAG, "Recebido %d byte(s) do master:", len);
        
        // /* 1) Bytes em hexadecimal */
        // ESP_LOG_BUFFER_HEX(TAG, rx_data, len);

        /* 2) Conteúdo recebido como string null-terminada (bytes crus) */
        int n = (len < (int)sizeof(printable) - 1) ? len : (int)sizeof(printable) - 1;
        memcpy(printable, rx_data, n);
        printable[n] = '\0';

        /* 3) Fatia o buffer em mensagens (o driver pode ter concatenado várias
           escritas do master) e despacha cada uma. A resposta ao master é servida
           continuamente por i2c_slave_request_task(), independente de comando. */
        ESP_LOGI(TAG, "Texto:  %s", printable);
        i2c_split_and_dispatch(printable);
    }
}
