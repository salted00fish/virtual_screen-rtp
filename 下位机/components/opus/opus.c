#include "opus.h"
#include "esp_log.h"
#include "audio_config.h"

static OpusDecoder *opus_dec_handle = NULL;

esp_err_t audio_opus_init(void)
{
    
    int err = 0;

    opus_dec_handle = opus_decoder_create(AUDIO_SAMPLE_RATE,AUDIO_CHANNELS,&err);

    if(err != 0 || opus_dec_handle == NULL)
    {
        ESP_LOGE("AUDIO", "opus_decoder_create failed: %d", err);
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t opus_to_pcm_decode(uint8_t *pdata_buf,uint32_t payload_len,uint8_t *decode_out_buf,uint32_t decode_out_len,uint32_t *out_bytes)
{
    if(!opus_dec_handle || !decode_out_buf || (payload_len > 0 && !pdata_buf))
    {
        return ESP_ERR_INVALID_ARG;
    }

    int ret = opus_decode(opus_dec_handle,pdata_buf,(int32_t)payload_len,(int16_t *)decode_out_buf,AUDIO_ONE_FRAMES,0);
    if(ret < 0)
    {
        ESP_LOGE("AUDIO", "opus_decode ret=%d len=%lu", ret, (unsigned long)payload_len);
        if(out_bytes)
            *out_bytes = 0;
        return ESP_FAIL;
    }

    if(out_bytes)
        *out_bytes = (uint32_t)(ret * 4); 


    return ESP_OK;
}

esp_err_t opus_plc_decode(uint8_t *decode_out_buf,uint32_t *out_bytes)
{
    if(opus_dec_handle == NULL || decode_out_buf == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    int ret = opus_decode(opus_dec_handle, NULL, 0, (int16_t *)decode_out_buf, AUDIO_ONE_FRAMES, 0);

    if(ret < 0)
    {
        ESP_LOGE("AUDIO", "opus plc ret=%d", ret);
        if(out_bytes)
            *out_bytes = 0;
        return ESP_FAIL;
    }

    if(out_bytes)
        *out_bytes = (uint32_t)(ret * 4);

    return ESP_OK;
}