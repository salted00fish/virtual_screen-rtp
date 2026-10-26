#ifndef JPEG_FULL_H
#define JPEG_FULL_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"


#define KCP_BUF_FREE_TIMEOUT_MS  (10) //500
#define KCP_BUF_FREE_EVENT_BIT   (1UL << 0)

typedef struct
{
    uint8_t *data_p;
    size_t data_len;
}jpeg_data_t;

esp_err_t jpeg_init(void);
esp_err_t jpeg_decode_show(uint8_t *pdata_buf,uint32_t jpeg_size);
#endif