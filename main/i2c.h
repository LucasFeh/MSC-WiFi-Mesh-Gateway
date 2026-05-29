#ifndef I2C_SLAVE_H
#define I2C_SLAVE_H

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* --- Barramento I2C em modo slave (API legada driver/i2c.h) --- */
#define I2C_SLAVE_PORT       0      /* I2C_NUM_0 */
#define I2C_SDA_PIN          21
#define I2C_SCL_PIN          22
#define I2C_SLAVE_ADDR       0x08   /* endereço de 7 bits */

#define I2C_RX_BUF_SIZE      256    /* buffer de recepção do driver (bytes) */
#define I2C_TX_BUF_SIZE      256    /* buffer de transmissão (exigido > 0 no modo slave) */
#define I2C_READ_TIMEOUT_MS  100    /* timeout de leitura da task dedicada */

#define MAX_MACS             50

/* Lista de MACs compartilhada com a task TX da mesh (mesh_main.c).
   Mantida aqui para preservar o link com o restante do firmware. */
extern uint8_t           i2c_macs[MAX_MACS][6];
extern volatile int      i2c_mac_count;
extern SemaphoreHandle_t i2c_macs_mutex;

void i2c_slave_init(void);
void i2c_slave_task(void *arg);

#endif /* I2C_SLAVE_H */
