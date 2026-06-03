#ifndef OTA_H_
#define OTA_H_
#include "flask_request.h"   /* MESH_TAG + esp_ota_ops.h / esp_mesh.h / esp_http_client.h */
#include "ota_protocol.h"    /* protocolo de fio compartilhado com o Driver               */


/* Dispara o OTA a partir de uma URL do Flask.
 *   target_mac != NULL -> UNICAST: envia o firmware só para esse MAC via mesh,
 *                         ignorando o roteamento por nome.
 *   target_mac == NULL -> roteia pelo NOME do arquivo:
 *       nome contém "Gateway" -> self-update do ROOT          (Fluxo A)
 *       nome contém "Driver"  -> repasse via mesh a todos os nós (Fluxo B)
 *       caso contrário        -> loga e ignora.
 * O log [OTA] com o destino é impresso ANTES de qualquer operação OTA. */
void trigger_ota(const char *url, const char *name, const uint8_t *target_mac);

/* Registra o OTA_ACK de um nó (chamado pelo RX da mesh, Fluxo B). */
void ota_root_register_ack(const uint8_t from_mac[6], uint8_t status);

#endif /* OTA_H_ */
