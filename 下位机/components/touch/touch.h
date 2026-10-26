#ifndef TOUCH_H
#define TOUCH_H

#include "esp_err.h"

esp_err_t touch_init(void);
void touch_get_kcp(void **p,void *kcp_lock);

#endif