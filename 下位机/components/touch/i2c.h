#ifndef I2C_H
#define I2C_H

#include "esp_err.h"

#define TOUCH_MAX_POINTS    5       /* GT9xxx 最多支持 5 点 */

esp_err_t touch_i2c_init(void);
int gt_poll_once(uint16_t *xs, uint16_t *ys);

#endif