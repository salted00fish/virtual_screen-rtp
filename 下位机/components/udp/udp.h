#ifndef UDP_H
#define UDP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "ikcp.h"

esp_err_t eth_udp_init(void);
esp_err_t udp_get_vidio_rx_queue(void *queue);
esp_err_t udp_get_audio_rx_queue(void *queue,void *kcp_queue);
int kcp_output(const char *buf, int len,ikcpcb *kcp, void *user);

void udp_get_kcp(void **p,void *kcp_lock);
#endif