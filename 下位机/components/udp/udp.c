#include "udp.h"
#include "lwip/udp.h"
#include "lwip/api.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "freertos/queue.h"

#define TAG "UDP"
static struct netconn* udp_netcon = NULL;
static struct netconn* audio_udp_netcon = NULL;

static SemaphoreHandle_t audio_udp_mutex = NULL;

void udp_recv_task(void *parm);
TaskHandle_t udp_recv_handle = NULL;

void audio_udp_recv_task(void *parm);
TaskHandle_t audio_udp_recv_handle = NULL;


void kcp_keep_task(void *parm);

static void lock(void)
{
    if(audio_udp_mutex) (void)xSemaphoreTake(audio_udp_mutex,portMAX_DELAY);
}

static void unlock(void)
{
    if(audio_udp_mutex) xSemaphoreGive(audio_udp_mutex);
}

esp_err_t eth_udp_init(void)
{
    esp_err_t err = ESP_OK;

    audio_udp_mutex = xSemaphoreCreateMutex();
    if(audio_udp_mutex == NULL)
    {
        ESP_LOGE(TAG,"互斥锁创建失败");
        err = ESP_ERR_NO_MEM;

        goto error;
    }

    
    udp_netcon = netconn_new(NETCONN_UDP);
    if(udp_netcon == NULL)
    {
        ESP_LOGE(TAG,"netconn_new error\r\n");
        err = ESP_ERR_NO_MEM;

        goto error;
    }

    err = netconn_bind(udp_netcon,IP_ADDR_ANY,5004);
    if(err !=ERR_OK)
    {
        ESP_LOGE(TAG,"bind error\r\n");
        goto error;
    }

    audio_udp_netcon = netconn_new(NETCONN_UDP);
    if(audio_udp_netcon == NULL)
    {
        ESP_LOGE(TAG,"audio netconn_new error\r\n");
        err = ESP_ERR_NO_MEM;

        goto error;
    }

    err = netconn_bind(audio_udp_netcon,IP_ADDR_ANY,5005);
    if(err !=ERR_OK)
    {
        ESP_LOGE(TAG,"audio bind error\r\n");
        goto error;
    }

    if(xTaskCreatePinnedToCore(udp_recv_task,"udp_recv_task",(1024*2)+512,NULL,10,&udp_recv_handle,0) != pdPASS)
    {
        ESP_LOGE(TAG,"udp_recv task create error\r\n");
        err = ESP_ERR_NO_MEM;
        goto error;
    }

    if(xTaskCreatePinnedToCore(audio_udp_recv_task,"audio_udp_recv_task",(1024*2)+512,NULL,11,&audio_udp_recv_handle,0) != pdPASS)
    {
        ESP_LOGE(TAG,"audio udp_recv task create error\r\n");
        err = ESP_ERR_NO_MEM;
        goto error;
    }

    if(xTaskCreatePinnedToCore(kcp_keep_task,"kcp_keep_task",(1024*2)+512,NULL,0,NULL,0) != pdPASS)
    {
        ESP_LOGE(TAG,"kcp_keep_task task create error\r\n");
        err = ESP_ERR_NO_MEM;
        goto error;
    }

    ESP_LOGE(TAG,"success\r\n");
    return ESP_OK;

error:
    if(udp_netcon)
    {
        netconn_delete(udp_netcon);
        udp_netcon = NULL;
    }

    if(audio_udp_netcon)
    {
        netconn_delete(audio_udp_netcon);
        audio_udp_netcon = NULL;
    }

    return err;
}

static QueueHandle_t video_rx_queue = NULL;
static QueueHandle_t audio_rx_queue = NULL;
static QueueHandle_t audio_kcp_rx_queue = NULL;

esp_err_t udp_get_vidio_rx_queue(void *queue)
{
    if(queue == NULL)
        return ESP_ERR_INVALID_ARG;
    
    video_rx_queue =(QueueHandle_t)queue;
    return ESP_OK;
}

esp_err_t udp_get_audio_rx_queue(void *queue,void *kcp_queue)
{
    if(queue == NULL || kcp_queue == NULL)
        return ESP_ERR_INVALID_ARG;
    
    
    audio_rx_queue =(QueueHandle_t)queue;
    audio_kcp_rx_queue = (QueueHandle_t)kcp_queue;
    return ESP_OK;
}

void udp_recv_task(void *parm)
{
    (void)parm;

    struct netbuf *recv_buf = NULL;
    err_t err = ERR_MEM;

    while(1)
    {
        err = netconn_recv(udp_netcon,&recv_buf);
        if(err == ERR_OK)
        {
            if(video_rx_queue && xQueueSend(video_rx_queue,&recv_buf,0) != pdPASS)
            {
                netbuf_delete(recv_buf);
            }
        }else
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

ip_addr_t remote;
uint16_t port = 0;
static volatile uint16_t kcp_keep_cnt = 0;

void audio_udp_recv_task(void *parm)
{
    (void)parm;

    struct netbuf *recv_buf = NULL;
    err_t err = ERR_MEM;

    while(1)
    {
        err = netconn_recv(audio_udp_netcon, &recv_buf);
        if(err == ERR_OK)
        {
            const ip_addr_t *ip = netbuf_fromaddr(recv_buf);

            if(ip)
            {
                remote = *ip;
            }
            port = netbuf_fromport(recv_buf);

            void *data_ptr;
            u16_t data_len;

            netbuf_data(recv_buf,&data_ptr,&data_len);
            


            bool is_routed = false; 

            if (data_len > 0)
            {
                uint8_t *payload = (uint8_t *)data_ptr;
                uint32_t kcp_conv = *((uint32_t *)payload);

                if(data_len >= 3 && payload[0] == 'F' && payload[1] == 'E' && payload[2] == 'C')
                {
                    if (audio_rx_queue && xQueueSend(audio_rx_queue,&recv_buf,0) == pdPASS)
                    {
                        is_routed = true;
                    }

                }else if(data_len >=4 && (payload[0] == 'P' && payload[1] == 'I' && payload[2] == 'N' && payload[3] == 'G'))
                {
                    kcp_keep_cnt = 0;
                }
                else if(data_len >= 12 && (payload[0] & 0xC0) == 0x80)
                {
                    uint8_t pt = payload[1] & 0x7F;
                    
                    if (pt == 96)
                    {
                        if (audio_rx_queue && xQueueSend(audio_rx_queue,&recv_buf,0) == pdPASS)
                        {
                            is_routed = true;
                        }
                    }

                }else if(data_len >= 24 && kcp_conv == 0x11223344 && (payload[4] >= 81 && payload[4] <= 84))
                {
                    if (audio_kcp_rx_queue && xQueueSend(audio_kcp_rx_queue, &recv_buf, 0) == pdPASS)
                    {
                        is_routed = true;
                    }

                }else
                {
                    ESP_LOGE(TAG,"error\r\n");
                }
            }

            if (!is_routed)
            {
                netbuf_delete(recv_buf);
            }
            
        }else
        {
            vTaskDelay(pdMS_TO_TICKS(1000)); // 或者处理断开逻辑
        }
    }
}



int kcp_output(const char *buf, int len,ikcpcb *kcp, void *user)
{
    (void)kcp;
    (void)user;

    lock();
    struct netbuf *send_buf = NULL;

    send_buf = netbuf_new();
    if(send_buf == NULL)
    {
        ESP_LOGE("udp_output task","netbuf_new error\r\n");
        unlock();
        return -1;
    }

    void *ptr = netbuf_alloc(send_buf,len);
    if(ptr == NULL)
    {
        ESP_LOGE("udp_output task","netbuf_alloc error\r\n");
        netbuf_delete(send_buf);
        unlock();
        return -1;
    }

    netbuf_take(send_buf,buf,len);

    netconn_sendto(audio_udp_netcon,send_buf,&remote,port);
    netbuf_delete(send_buf);

    unlock();
	return 0;
}

static ikcpcb **udp_kcp = NULL;
SemaphoreHandle_t udp_kcp_mutex = NULL;

void udp_get_kcp(void **p,void *kcp_lock)
{
    udp_kcp = (ikcpcb **)p;
    udp_kcp_mutex = (SemaphoreHandle_t)kcp_lock;

    if(udp_kcp_mutex == NULL)
    {
        ESP_LOGE("UDP","udp_get_kcp error");
    }
}


void kcp_keep_task(void *parm)
{
    (void)parm;

    char buf[] = {'P','I','N','G'};

    TickType_t xLastWakeTime = xTaskGetTickCount();
    while(1)
    {
        if(ip_addr_isany_val(remote) || port == 0)
        {

            vTaskDelayUntil(&xLastWakeTime,pdMS_TO_TICKS(1000));
            continue;
        }

        kcp_output(buf,sizeof(buf)/sizeof(buf[0]),NULL,NULL);
        kcp_keep_cnt++;

        if(kcp_keep_cnt == 5)
        {
            if(udp_kcp_mutex)
            {
                xSemaphoreTake(udp_kcp_mutex,portMAX_DELAY);
                if(*udp_kcp)
                {
                    ikcp_release(*udp_kcp);
                    *udp_kcp = NULL; 
                }

                *udp_kcp = ikcp_create(0x11223344,NULL);

                if(*udp_kcp != NULL)
                {
                    ikcp_setoutput(*udp_kcp,kcp_output);
                    ikcp_wndsize(*udp_kcp,16,16);
                    (*udp_kcp)->rcv_wnd = 16;
                    ikcp_nodelay(*udp_kcp,1,10,2,1);
                    ESP_LOGE("UDP","ikcp_create success");
                }else
                {
                    ESP_LOGE("UDP","ikcp_create error");
                }

                xSemaphoreGive(udp_kcp_mutex);
                
            }
        }

        vTaskDelayUntil(&xLastWakeTime,pdMS_TO_TICKS(200));
    }
}