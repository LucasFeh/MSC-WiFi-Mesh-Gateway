#ifndef OTA_H_
#define OTA_H_


#include "flask_request.h"

/* ---- Tipos de mensagem usados na comunicação mesh ---- */
typedef enum {
    BIN_MSG_FW_PACKET = 0x0102,
} BIN_MSG_ID_t;

/* Pacote de firmware enviado pela mesh durante o OTA */
typedef struct {
    uint16_t id;
    char     version[32];
    uint32_t offset;
    uint32_t data_size;
    uint32_t total_size;
    uint8_t  data[1024];
} __attribute__((packed)) firmware_packet_t;

extern void process_firmware_packet(firmware_packet_t *pkt);
extern void trigger_ota(const char *url);

#endif /* OTA_H_ */