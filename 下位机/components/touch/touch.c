#include "touch.h"
#include "i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include <inttypes.h>
#include <string.h>
#include "ikcp.h"

#define TAG "TOUCH"

#define SHORT_PRESS_CNT     3
#define LONG_PRESS_CNT      60
#define PRESS_DOUBLE_TIMER  50
#define SLIDE_STEP          20       /* 每 20px 上报一次 */
#define SLIDE_THRESHOLD     15      /* 总位移超 15px 才算滚动 */

#define CLICK_TYPE               0x01
#define LONG_CLICK_TYPE          0x02
#define DOUBLE_CLICK_TYPE        0x03
#define MOVE_TYPE                0x04
#define MOVEING_TYPE             0x05
#define SLIDE_END_TYPE           0x06

typedef struct 
{
    uint16_t x;
    uint16_t y;
    uint32_t type;
}touch_t;

static touch_t touch = {};

static ikcpcb **touch_kcp = NULL;
SemaphoreHandle_t kcp_mutex = NULL;



static void touch_send(touch_t *touch);

static void touch_poll_task(void *arg)
{
    (void)arg;

    uint16_t xs[TOUCH_MAX_POINTS], ys[TOUCH_MAX_POINTS];
    int touch_point = 0;

    uint32_t press_cnt = 0;
    uint8_t press_lock = false;
    uint8_t short_press_flag = false;
    uint8_t long_press_flag = false;

    uint16_t click_press_cnt = 0;
    uint32_t click_timer_timer = 0;

    uint8_t  sliding  = false;
    uint16_t down_x   = 0, down_y = 0; /* 按下起点 */
    uint16_t last_x   = 0, last_y = 0; /* 上次上报位置 */

    while (1)
    {
        touch_point = gt_poll_once(xs,ys);

        if(touch_point == -1)
        {
            ESP_LOGE(TAG,"touch_point I2C error");
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        if(touch_point == 0)
        {
            if(sliding)                       /* 滚动结束 */
            {
                touch.x    = xs[0];
                touch.y    = ys[0];
                touch.type = SLIDE_END_TYPE;
                touch_send(&touch);

                sliding           = false;
                short_press_flag  = false;
                long_press_flag   = false;
                click_press_cnt   = 0;
                click_timer_timer = 0;

            }else if(long_press_flag)
            {
                long_press_flag = false;
                click_timer_timer = 0;//长按就别触发单击或者双击了
                click_press_cnt = 0;
                
                touch.x = xs[0];
                touch.y = ys[0];
                touch.type = LONG_CLICK_TYPE;
                touch_send(&touch);

            }else if(click_press_cnt == 2)//双击
            {
                short_press_flag = false;
                click_timer_timer = 0;
                click_press_cnt = 0;

                touch.x = xs[0];
                touch.y = ys[0];
                touch.type = DOUBLE_CLICK_TYPE;
                touch_send(&touch);

            }else if(click_press_cnt > 0)//判断是否双击
            {
                click_timer_timer++;
                if(click_timer_timer >= PRESS_DOUBLE_TIMER)//如果大于双击的时间，没按下n击，默认是单击
                {
                    if(click_press_cnt == 1)//单击
                    {
                        short_press_flag = false;

                        touch.x = xs[0];
                        touch.y = ys[0];
                        touch.type = CLICK_TYPE;
                        touch_send(&touch);
                    }

                    click_timer_timer = 0;
                    click_press_cnt = 0;
                }
            }

            press_lock = false;
            press_cnt = 0;

        }else if(!press_lock && touch_point == 1)
        {
            press_cnt++;

            if(press_cnt >= SHORT_PRESS_CNT)
            {
                click_press_cnt++;
                click_timer_timer = 0;

                down_x = xs[0];
                down_y = ys[0];
                last_x = down_x;
                last_y = down_y;

                short_press_flag  = true;
                press_lock = true;
            }
            
        }else if(touch_point == 1)
        {
            /* ---- 1. 滚动检测 ---- */
            if (!sliding && !long_press_flag &&
                (abs((int)xs[0] - (int)down_x) >= SLIDE_THRESHOLD ||
                 abs((int)ys[0] - (int)down_y) >= SLIDE_THRESHOLD))
            {
                sliding           = true;
                short_press_flag  = false;
                click_press_cnt   = 0;                /* 滚动不算点击 */
                click_timer_timer = 0;
                long_press_flag   = false;
                press_cnt         = LONG_PRESS_CNT;   /* 长按锁死 */

                last_x = xs[0];
                last_y = ys[0];

                touch.x    = xs[0];
                touch.y    = ys[0];
                touch.type = MOVE_TYPE;
                touch_send(&touch);

            }else if (sliding)
            {
                int dx = (int)xs[0] - (int)last_x;
                int dy = (int)ys[0] - (int)last_y;

                if (abs(dx) >= SLIDE_STEP || abs(dy) >= SLIDE_STEP)
                {
                    last_x = xs[0];
                    last_y = ys[0];

                    touch.x    = xs[0];
                    touch.y    = ys[0];
                    touch.type = MOVEING_TYPE;
                    touch_send(&touch);
                }
            }
            else if (press_cnt < LONG_PRESS_CNT)
            {
                press_cnt++;

                if (press_cnt >= LONG_PRESS_CNT)
                {
                    press_cnt        = 0;
                    short_press_flag = false;
                    long_press_flag  = true;
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t touch_init(void)
{
    esp_err_t err;

    err = touch_i2c_init();

    if(err != ESP_OK)
    {
        ESP_LOGE(TAG,"touch_i2c_init error");

        return err;
    }

    if(xTaskCreatePinnedToCore(touch_poll_task,"touch_poll_task",1024*2,NULL,1,NULL,0) != pdPASS)
    {
        err = ESP_ERR_NO_MEM;
        return err;
    }

    return ESP_OK;
}

void touch_get_kcp(void **p,void *kcp_lock)
{
    touch_kcp = (ikcpcb **)p;
    kcp_mutex = (SemaphoreHandle_t)kcp_lock;
    if(touch_kcp == NULL || kcp_mutex == NULL || *touch_kcp)
    {
        ESP_LOGE(TAG,"touch_kcp error");
    }
}

static void touch_send(touch_t *touch)
{
    if(!kcp_mutex)
    {
        ESP_LOGE(TAG,"touch_send  kcp_mutex error");
        return;
    }
    xSemaphoreTake(kcp_mutex,portMAX_DELAY);
    if(*touch_kcp != NULL)
    {
        ikcp_send(*touch_kcp,(char*)touch,sizeof(touch_t));
    }else
    {
        ESP_LOGE(TAG,"touch_send error");
    }
    
    xSemaphoreGive(kcp_mutex);
}