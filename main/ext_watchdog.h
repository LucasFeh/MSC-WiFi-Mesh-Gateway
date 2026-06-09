#pragma once
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================
 *  Watchdog externo de hardware — PORTE FIEL do original (src/main.cpp:423-430).
 *  Uma task dedicada alterna o GPIO26 de forma INCONDICIONAL: ~50 ms em HIGH e
 *  ~500 ms em LOW (no original: timeOff=500, e timeOff*0.10=50). Esse é o sinal
 *  já comprovado na placa real; alimenta o WDI do IC de watchdog externo.
 *
 *  Decisão de projeto: mantido INCONDICIONAL como no original (sem health-gate).
 * ======================================================================== */

#define WDT_GPIO_PIN         GPIO_NUM_26  /* WDI do IC externo (orig: WDcontrol=26) */
#define WDT_KICK_PERIOD_MS   500          /* LOW=500ms, HIGH=50ms (orig: timeOff=500) */

/* Configura o GPIO de kick como saída. Chamar no início de app_main(). */
void ext_wdt_init(void);

/* Cria a task de kick (maior prioridade). Chamar após ext_wdt_init(). */
void ext_wdt_start(void);

#ifdef __cplusplus
}
#endif
