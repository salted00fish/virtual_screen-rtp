#ifndef OPUS_H
#define OPUS_H

#include "esp_err.h"
#include "esp_audio_dec.h"
#include "esp_audio_dec_default.h"
#include "esp_opus_dec.h"

typedef struct OpusDecoder OpusDecoder;
OpusDecoder *opus_decoder_create(int32_t Fs, int channels, int *error);
int opus_decode(OpusDecoder *st, const unsigned char *data,
                int32_t len, int16_t *pcm, int frame_size, int decode_fec);

esp_err_t audio_opus_init(void);
esp_err_t opus_to_pcm_decode(uint8_t *pdata_buf,uint32_t payload_len,uint8_t *decode_out_buf,uint32_t decode_out_len,uint32_t *out_bytes);
esp_err_t opus_plc_decode(uint8_t *decode_out_buf,uint32_t *out_bytes);
#endif