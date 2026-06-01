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

/* Definições dos símbolos declarados como extern em i2c.h.
   São consumidos pela task TX da mesh (mesh_main.c); mantidos aqui
   para preservar o link, mesmo que esta versão não popule a lista. */
uint8_t           i2c_macs[MAX_MACS][6];
volatile int      i2c_mac_count = 0;
SemaphoreHandle_t i2c_macs_mutex = NULL;

/* --- Control surface: flags/estado setados pelos comandos recebidos via I2C.
   Devem ser consumidos pela lógica da mesh/aplicação (ver i2c.h). --- */
volatile bool RequestSendFlag      = false;
volatile bool clearMacs            = false;
volatile bool flagCommit           = false;
volatile bool flagUnCommit         = false;
volatile bool flagUniscastUpdate   = false;
volatile bool flagUpdate           = false;
volatile bool flagReboot           = false;
volatile bool AP_FLAG              = false;
volatile bool timerAdjust          = false;
volatile int  seconds              = 0;
char          numero[8]            = {0};   /* nº de série do comando UPDATE */
char          updateUnicastMacStr[18] = {0};

void i2c_slave_init(void)
{
    ESP_LOGI(TAG, "Inicializando I2C slave (API legada driver/i2c.h)...");

    i2c_macs_mutex = xSemaphoreCreateMutex();

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
        RequestSendFlag = true;
        return;
    }

    if (strcmp(buffer, "CLEAR") == 0) {
        clearMacs = true;
        return;
    }

    if (strcmp(buffer, "COMMIT") == 0) {
        RequestSendFlag = true;
        flagCommit = true;
        return;
    }

    if (strcmp(buffer, "UNCOMMIT") == 0) {
        RequestSendFlag = true;
        flagUnCommit = true;
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
        /* TODO: SensorData[] não existe neste projeto. O original fazia:
           memcpy(SensorData[macCount].MAC, newMac, 6); */
        ESP_LOGI(TAG, "MAC %d armazenado: %s", i2c_mac_count, buffer);
        i2c_mac_count++;
    } else {
        ESP_LOGI(TAG, "Limite de MACs atingido");
    }

    if (i2c_macs_mutex) xSemaphoreGive(i2c_macs_mutex);
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

        /* ---- Dados válidos recebidos (len > 0) ---- */
        ESP_LOGI(TAG, "Recebido %d byte(s) do master:", len);
        
        // /* 1) Bytes em hexadecimal */
        // ESP_LOG_BUFFER_HEX(TAG, rx_data, len);

        /* 2) Conteúdo recebido como string null-terminada (bytes crus) */
        int n = (len < (int)sizeof(printable) - 1) ? len : (int)sizeof(printable) - 1;
        memcpy(printable, rx_data, n);
        printable[n] = '\0';

        /* 3) Trata o comando recebido (dispatcher portado do onReceive Arduino) */
        
        ESP_LOGI(TAG, "Texto:  %s", printable);
        i2c_on_receive(printable);
    }
}
