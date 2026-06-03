#ifndef OTA_PROTOCOL_H_
#define OTA_PROTOCOL_H_

/* ===========================================================================
 *  Protocolo de fio do OTA via ESP-WIFI-MESH  (FONTE ÚNICA DE VERDADE)
 *
 *  Este arquivo é COMPARTILHADO, byte a byte, entre o repo ROOT/Gateway e o
 *  repo NODE/Driver. Qualquer mudança aqui deve ser replicada nos dois.
 *
 *  Substitui o antigo BIN_MSG_FW_PACKET, que tinha valores divergentes entre
 *  os repos (0x0102 no ota.h do root vs 0x0001 nos headers mesh) — causa de o
 *  nó nunca reconhecer os pacotes de firmware.
 *
 *  Referência ESP-WIFI-MESH (esp_mesh_send / esp_mesh_recv / routing table):
 *  https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_mesh.html
 * ===========================================================================
 */

#include <stdint.h>
#include <stddef.h>   /* offsetof */

/* IDs de mensagem mesh (campo msg_id, sempre os 2 primeiros bytes do payload) */
#define BIN_MSG_OTA       0x0010   /* ROOT -> NODE : pacote de firmware           */
#define BIN_MSG_OTA_ACK   0x0011   /* NODE -> ROOT : confirmação de entrega       */

/* Tamanho máx. de um chunk. O pacote inteiro (header + payload) precisa caber
 * num único esp_mesh_send(): ESP-WIFI-MESH garante entrega confiável só
 * fragmento a fragmento, então mantemos < 1400 bytes.
 *   sizeof(ota_packet_t) = 2+1(+1 pad)+4+4+4+4 + 1024 = ~1044 bytes < 1400.  */
#define OTA_CHUNK_MAX     1024

/* tipo do pacote dentro do fluxo OTA */
typedef enum {
    OTA_BEGIN = 0,   /* primeiro pacote: abre a partição (carrega 'total')      */
    OTA_CHUNK = 1,   /* pacote intermediário                                    */
    OTA_END   = 2,   /* último pacote: finaliza, valida e marca boot            */
} ota_type_t;

/* Pacote de firmware ROOT -> NODE. Campos pedidos pelo protocolo:
 *   tipo, total, offset, chunk_size, payload, crc.                              */
typedef struct __attribute__((packed)) {
    uint16_t msg_id;                 /* = BIN_MSG_OTA                            */
    uint8_t  tipo;                   /* ota_type_t                               */
    uint32_t total;                  /* tamanho total do firmware (bytes)        */
    uint32_t offset;                 /* posição deste chunk no firmware          */
    uint32_t chunk_size;             /* bytes válidos em payload[]               */
    uint32_t crc;                    /* CRC32 do payload (validação por chunk)   */
    uint8_t  payload[OTA_CHUNK_MAX];
} ota_packet_t;

/* Tamanho do cabeçalho (sem payload): pacotes de controle BEGIN/END são
 * enviados só com o cabeçalho, sem arrastar os 1024 bytes vazios pela mesh. */
#define OTA_HDR_SIZE   (offsetof(ota_packet_t, payload))

/* O pacote OTA_BEGIN leva, em payload[], o NOME do arquivo .bin que o Gateway
 * recebeu do dashboard (string null-terminated, ex.: "Driver-1.bin"), com
 * chunk_size = strlen+1. É o mesmo nome usado no roteamento do Gateway (nomes
 * "Gateway" ou "Driver"); o NODE o usa p/ recusar firmware do canal errado.
 * CHUNK/END seguem inalterados — só o BEGIN passa a trafegar essa string. */
#define OTA_FW_NAME_MAX   64

/* Confirmação NODE -> ROOT, enviada após esp_ota_end()/set_boot e antes do
 * reboot. O MAC de origem vem do parâmetro 'from' de esp_mesh_recv().           */
typedef struct __attribute__((packed)) {
    uint16_t msg_id;                 /* = BIN_MSG_OTA_ACK                        */
    uint8_t  status;                 /* OTA_ACK_OK / OTA_ACK_FAIL                */
} ota_ack_t;

#define OTA_ACK_FAIL  0
#define OTA_ACK_OK    1

#endif /* OTA_PROTOCOL_H_ */
