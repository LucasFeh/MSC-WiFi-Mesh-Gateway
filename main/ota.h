#ifndef OTA_H_
#define OTA_H_
#include "flask_request.h"   /* MESH_TAG + esp_ota_ops.h / esp_mesh.h / esp_http_client.h */
#include "ota_protocol.h"    /* protocolo de fio compartilhado com o Driver               */


/* Dispara o OTA a partir de uma URL do Flask, roteando pelo NOME do arquivo:
 *   nome contém "Gateway" -> self-update do ROOT          (Fluxo A)
 *   nome contém "Driver"  -> repasse via mesh aos nós      (Fluxo B)
 *   caso contrário        -> loga e ignora.
 * O log [OTA] com o destino é impresso ANTES de qualquer operação OTA. */
void trigger_ota(const char *url, const char *name);

/* Registra o OTA_ACK de um nó (chamado pelo RX da mesh, Fluxo B). */
void ota_root_register_ack(const uint8_t from_mac[6], uint8_t status);

#endif /* OTA_H_ */
