#ifndef ESP_ETH_H
#define ESP_ETH_H

#include <IPAddress.h>

typedef enum {
    ETH_FAIL_NONE = 0,
    ETH_FAIL_BEGIN,
    ETH_FAIL_LINK_TIMEOUT,
    ETH_FAIL_IP_TIMEOUT,
    ETH_FAIL_STATIC_IP_FAILED
} EthFailReason;

bool eth_setup(void);
bool eth_begin_runtime(void);
bool eth_is_connected(void);
bool eth_started(void);
bool eth_link_is_up(void);
EthFailReason eth_last_fail_reason(void);
const char *eth_last_fail_reason_text(void);
const char *eth_mode_text(void);
IPAddress eth_current_ip(void);
IPAddress eth_gateway_ip(void);
IPAddress eth_subnet_mask(void);
IPAddress eth_dns_ip(void);
const char *eth_mac_string(void);
uint8_t eth_link_speed_mbps(void);
bool eth_is_full_duplex(void);

#endif /* ESP_ETH_H */
