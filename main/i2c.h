#ifndef I2C_SLAVE_H
#define I2C_SLAVE_H

#include <stdint.h>
#include <stdbool.h>
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

#define MAX_MACS             34

/* Lista de MACs compartilhada com a task TX da mesh (mesh_main.c).
   Mantida aqui para preservar o link com o restante do firmware. */
extern uint8_t           i2c_macs[MAX_MACS][6];
extern volatile int      i2c_mac_count;
extern SemaphoreHandle_t i2c_macs_mutex;

/* --- Control surface: flags/estado setados pelos comandos I2C (i2c_on_receive).
   Devem ser lidos/consumidos pela lógica da mesh/aplicação. --- */
extern volatile bool RequestSendFlag;        /* CLICKED/COMMIT/... pediram envio */
extern volatile bool clearMacs;              /* CLEAR */
extern volatile bool flagCommit;             /* COMMIT */
extern volatile bool flagUnCommit;           /* UNCOMMIT */
extern volatile bool flagUniscastUpdate;     /* OPDATEUNICAST{AP,STA} */
extern volatile bool flagUpdate;             /* UPDATE */
extern volatile bool flagReboot;             /* REBOOT */
extern volatile bool AP_FLAG;                /* modo AP x STA */
extern volatile bool timerAdjust;            /* TIME:xx */
extern volatile int  seconds;                /* valor de TIME:xx */
extern char          numero[8];              /* nº de série (UPDATE) */
extern char          updateUnicastMacStr[18];/* MAC alvo do update unicast */

void i2c_slave_init(void);
void i2c_slave_task(void *arg);

#endif /* I2C_SLAVE_H */
