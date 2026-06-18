#ifndef I2C_SLAVE_H
#define I2C_SLAVE_H

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* --- Barramento I2C em modo slave (API legada driver/i2c.h) ---
   Após a migração para MQTT, o I2C ficou SÓ como canal de reboot do root:
   o gateway escreve "UPDT_I2C" e o root chama esp_restart(). Leituras e demais
   comandos agora trafegam por MQTT. */
#define I2C_SLAVE_PORT       0      /* I2C_NUM_0 */
#define I2C_SDA_PIN          21
#define I2C_SCL_PIN          22
#define I2C_SLAVE_ADDR       0x08   /* endereço de 7 bits */

#define I2C_RX_BUF_SIZE      256    /* buffer de recepção do driver (bytes) */
#define I2C_READ_TIMEOUT_MS  100    /* timeout de leitura da task dedicada */

#define MAX_MACS             34

/* Lista de MACs compartilhada com a task TX da mesh (mesh_main.c). Agora é
   populada via MQTT (mesh/cmd/maclist), não mais por I2C. */
extern uint8_t           i2c_macs[MAX_MACS][6];
extern volatile int      i2c_mac_count;
extern SemaphoreHandle_t i2c_macs_mutex;

extern volatile bool pending_read_broadcast;
extern volatile bool pending_reboot;
/* Leituras de mesh por-MAC (alinhadas por índice com i2c_macs[]). Populadas no RX
   da mesh (BIN_MSG_READ_RESPONSE); rTCounter conta ciclos de broadcast sem
   resposta — ao atingir 3 o sensor é reportado zerado. Protegidas por i2c_macs_mutex. */
typedef struct {
    uint8_t ch1;
    uint8_t ch2;
    uint8_t ch3;
    float tensao;
    uint8_t rTCounter;   /* staleness: 0 = leitura fresca, >=3 = offline */
} i2c_reading_t;

extern i2c_reading_t i2c_readings[MAX_MACS];

/* --- Estado herdado (mantido p/ compatibilidade de link). 'seconds' ainda é
   consumido por read_timer (mesh_main.c); agora é ajustado via mesh/cmd/time. --- */
extern volatile bool RequestSendFlag;
extern volatile bool clearMacs;
extern volatile bool flagCommit;
extern volatile bool flagUnCommit;
extern volatile bool flagUniscastUpdate;
extern volatile bool flagUpdate;
extern volatile bool flagReboot;
extern volatile bool AP_FLAG;
extern volatile bool timerAdjust;
extern volatile int  seconds;                /* período de leitura (segundos) */
extern char          numero[8];
extern char          updateUnicastMacStr[18];

void i2c_slave_init(void);
void i2c_slave_task(void *arg);          /* RX: só reboot do root (UPDT_I2C) */

#endif /* I2C_SLAVE_H */
