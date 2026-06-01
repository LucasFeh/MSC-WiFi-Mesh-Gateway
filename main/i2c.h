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
#define I2C_READ_TIMEOUT_MS  100    /* timeout de leitura da task dedicada */

#define MAX_MACS             34

/* Caminho slave -> master (onRequest): o master fixo lê I2C_REQUEST_STRIDE bytes
   por requestFrom e descarta padding 0xFF/0x8f. Cada registro (1 JSON, "Vazio"
   ou "FIM") é emitido com exatamente esse stride, de modo que o FIFO TX entregue
   um registro por leitura sem precisar de callback. */
#define I2C_REQUEST_STRIDE   72

/* Profundidade do ring buffer TX em REGISTROS. A task escreve um registro por vez
   e bloqueia quando cheio (backpressure por-registro), de modo que o atraso entre
   um dado novo e o que o master lê fica limitado a ~esta quantidade de leituras —
   independente do tamanho da lista. Mantê-lo PEQUENO evita acumular cópias velhas
   do blob na fila (era a causa do "Vazio" repetido no boot). Alguns registros de
   folga evitam underrun do FIFO de hardware sob carga. */
#define I2C_TX_DEPTH_RECORDS 8
#define I2C_TX_BUF_SIZE      (I2C_TX_DEPTH_RECORDS * I2C_REQUEST_STRIDE)

/* Maior blob possível montado de uma vez: um registro por MAC + o sentinela "FIM". */
#define I2C_REQUEST_BLOB_MAX ((MAX_MACS + 1) * I2C_REQUEST_STRIDE)

/* Lista de MACs compartilhada com a task TX da mesh (mesh_main.c).
   Mantida aqui para preservar o link com o restante do firmware. */
extern uint8_t           i2c_macs[MAX_MACS][6];
extern volatile int      i2c_mac_count;
extern SemaphoreHandle_t i2c_macs_mutex;

/* Leituras de mesh por-MAC (alinhadas por índice com i2c_macs[]), consumidas pelo
   onRequest I2C. Populadas no RX da mesh (BIN_MSG_READ_RESPONSE); rTCounter conta
   ciclos de broadcast sem resposta — ao atingir 3 o sensor é reportado zerado.
   Protegidas pelo mesmo i2c_macs_mutex. */
typedef struct {
    uint8_t ch1;
    uint8_t ch2;
    uint8_t ch3;
    uint8_t tensao;      /* sem campo na mesh ainda: mantido em 0 */
    uint8_t rTCounter;   /* staleness: 0 = leitura fresca, >=3 = offline */
} i2c_reading_t;

extern i2c_reading_t i2c_readings[MAX_MACS];

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
void i2c_slave_task(void *arg);          /* RX: master -> slave (comandos) */
void i2c_slave_request_task(void *arg);  /* TX: slave -> master (onRequest contínuo) */

#endif /* I2C_SLAVE_H */
