#include "led.h"
#include <stdint.h>
#include "driver/gpio.h"
#include "esp_check.h"

#define LED_PIN             GPIO_NUM_42   // LED2 -> GPIO42

static const char *TAG = "led";

esp_err_t led_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "GPIO%d config failed", (int)LED_PIN);
    return gpio_set_level(LED_PIN, 0);
}

void led_toggle(void)
{
    static uint8_t status = 0;
    gpio_set_level(LED_PIN,status);
    status = !status;
}