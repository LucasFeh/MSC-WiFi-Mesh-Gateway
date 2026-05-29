#include "i2c.h"
#include "driver/i2c.h"        /* API legada (não usar driver/i2c_slave.h) */
#include "esp_log.h"
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

static const char *TAG = "I2C_SLAVE";

/* Definições dos símbolos declarados como extern em i2c.h.
   São consumidos pela task TX da mesh (mesh_main.c); mantidos aqui
   para preservar o link, mesmo que esta versão não popule a lista. */
uint8_t           i2c_macs[MAX_MACS][6];
volatile int      i2c_mac_count = 0;
SemaphoreHandle_t i2c_macs_mutex = NULL;

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

/* Compara o conteúdo recebido com um comando ASCII, ignorando
   espaços e terminadores (\0 \r \n \t e espaço) ao final. */
static bool match_command(const uint8_t *buf, int len, const char *cmd)
{
    while (len > 0) {
        uint8_t c = buf[len - 1];
        if (c == '\0' || c == '\r' || c == '\n' || c == '\t' || c == ' ')
            len--;
        else
            break;
    }
    size_t cmd_len = strlen(cmd);
    return ((size_t)len == cmd_len) && (memcmp(buf, cmd, cmd_len) == 0);
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

        /* 1) Bytes em hexadecimal */
        ESP_LOG_BUFFER_HEX(TAG, rx_data, len);

        /* 2) Conteúdo interpretado como string (não-imprimíveis viram '.') */
        for (int i = 0; i < len; i++) {
            printable[i] = isprint((unsigned char)rx_data[i]) ? (char)rx_data[i] : '.';
        }
        printable[len] = '\0';
        ESP_LOGI(TAG, "Texto: \"%s\"", printable);

        /* 3) Comando "iniciar" */
        if (match_command(rx_data, len, "iniciar")) {
            ESP_LOGW(TAG, ">>> Comando 'iniciar' reconhecido! Iniciando rotina. <<<");
        }
    }
}
