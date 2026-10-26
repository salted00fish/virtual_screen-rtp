#ifndef LED_H
#define LED_H

#include "esp_err.h"

esp_err_t led_init(void);
void led_toggle(void);

#endif