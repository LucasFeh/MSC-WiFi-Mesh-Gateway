#include "ext_watchdog.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *WDT_TAG = "WDT";

/* Maior que MPTX/MPRX (prio 5, main.h:197-198) e LED (prio 3, DriverHandle.cpp:66). */
#define WDT_TASK_PRIORITY  (configMAX_PRIORITIES - 1)
#define WDT_TASK_STACK     2048

/* Porte fiel de wdtimer() (src/main.cpp:423-430): alterna o GPIO de forma
 * incondicional — HIGH por 50 ms (WDT_KICK_PERIOD_MS/10) e LOW por 500 ms
 * (WDT_KICK_PERIOD_MS). O nível é mantido em variável local (em vez de reler o
 * pino de saída), equivalente ao digitalRead(WDcontrol) do original. */
static void wdt_kick_task(void *arg)
{
    int level = 0;
    for (;;)
    {
        level = !level;
        gpio_set_level(WDT_GPIO_PIN, level);
        vTaskDelay(pdMS_TO_TICKS(level ? (WDT_KICK_PERIOD_MS / 10) : WDT_KICK_PERIOD_MS));
    }
}

void ext_wdt_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << WDT_GPIO_PIN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(WDT_GPIO_PIN, 0);   /* nível inicial LOW, como o original */

    ESP_LOGI(WDT_TAG, "[WDT] Watchdog externo (porte fiel) - GPIO %d, HIGH %dms / LOW %dms",
             (int)WDT_GPIO_PIN, WDT_KICK_PERIOD_MS / 10, WDT_KICK_PERIOD_MS);
}

void ext_wdt_start(void)
{
    xTaskCreate(wdt_kick_task, "WDT_KICK", WDT_TASK_STACK, NULL, WDT_TASK_PRIORITY, NULL);
}
