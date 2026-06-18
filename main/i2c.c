#include "i2c.h"
#include "driver/i2c.h"        /* API legada (não usar driver/i2c_slave.h) */
#include "esp_log.h"
#include "esp_system.h"        /* esp_restart() */
#include <string.h>

static const char *TAG = "I2C_SLAVE";

/* Definições consumidas pela task TX/RX da mesh (mesh_main.c). A lista de MACs
   agora é populada via MQTT (mesh/cmd/maclist), não mais por I2C. */
uint8_t           i2c_macs[MAX_MACS][6];
volatile int      i2c_mac_count = 0;
SemaphoreHandle_t i2c_macs_mutex = NULL;
i2c_reading_t     i2c_readings[MAX_MACS] = {0};

/* Estado herdado: mantido p/ compatibilidade de link. 'seconds' é ajustado via
   mesh/cmd/time (flask_request.c) e consumido por read_timer (mesh_main.c).
   Os demais flags não são mais setados (eram do dispatcher I2C removido). */
volatile bool RequestSendFlag      = false;
volatile bool flagCommit           = false;
volatile bool flagUnCommit         = false;
volatile bool flagUniscastUpdate   = false;
volatile bool flagUpdate           = false;
volatile bool flagReboot           = false;
volatile bool AP_FLAG              = false;
volatile bool timerAdjust          = false;
volatile int  seconds              = 0;
char          numero[8]            = {0};
char          updateUnicastMacStr[18] = {0};

void i2c_slave_init(void)
{
    ESP_LOGI(TAG, "Inicializando I2C slave (reboot-only, API legada)...");

    i2c_macs_mutex = xSemaphoreCreateMutex();

    i2c_config_t conf = {
        .mode                = I2C_MODE_SLAVE,
        .sda_io_num          = I2C_SDA_PIN,
        .scl_io_num          = I2C_SCL_PIN,
        .sda_pullup_en       = GPIO_PULLUP_ENABLE,
        .scl_pullup_en       = GPIO_PULLUP_ENABLE,
        .slave.addr_10bit_en = 0,
        .slave.slave_addr    = I2C_SLAVE_ADDR,
        .clk_flags           = 0,
    };

    ESP_ERROR_CHECK(i2c_param_config(I2C_SLAVE_PORT, &conf));
    /* TX buffer = 0: a slave não responde mais leituras (sem onRequest de 72 B). */
    ESP_ERROR_CHECK(i2c_driver_install(I2C_SLAVE_PORT, conf.mode,
                                       I2C_RX_BUF_SIZE, 0, 0));

    ESP_LOGI(TAG, "I2C slave pronto: addr=0x%02X  SDA=%d  SCL=%d  (só comando UPDT_I2C)",
             I2C_SLAVE_ADDR, I2C_SDA_PIN, I2C_SCL_PIN);
}

void i2c_slave_task(void *arg)
{
    uint8_t rx_data[I2C_RX_BUF_SIZE];
    char    printable[I2C_RX_BUF_SIZE + 1];

    ESP_LOGI(TAG, "Task I2C (reboot-only) iniciada (timeout %d ms)", I2C_READ_TIMEOUT_MS);

    while (1) {
        int len = i2c_slave_read_buffer(I2C_SLAVE_PORT, rx_data, sizeof(rx_data),
                                        pdMS_TO_TICKS(I2C_READ_TIMEOUT_MS));
        if (len <= 0) continue;

        int n = (len < (int)sizeof(printable) - 1) ? len : (int)sizeof(printable) - 1;
        memcpy(printable, rx_data, n);
        printable[n] = '\0';

        /* Único comando mantido por I2C: reboot do root (recuperação out-of-band). */
        if (strstr(printable, "UPDT_I2C")) {
            ESP_LOGW(TAG, "[I2C] UPDT_I2C -> esp_restart()");
            esp_restart();
        }
    }
}
