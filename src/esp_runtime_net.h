#ifndef ESP_RUNTIME_NET_H
#define ESP_RUNTIME_NET_H

#include <IPAddress.h>
#include <stdint.h>

typedef enum {
    RUNTIME_NET_NONE = 0,
    RUNTIME_NET_WIFI_STA,
    RUNTIME_NET_WIFI_AP,
    RUNTIME_NET_ETH
} RuntimeNetMode;

bool runtime_net_begin(void);
void runtime_net_poll(void);

RuntimeNetMode runtime_net_mode(void);
const char *runtime_net_mode_text(void);
bool runtime_net_has_ip(void);
bool runtime_net_is_ready_for_web(void);
bool runtime_net_should_run_bode(void);
bool runtime_net_recovery_ap_active(void);
bool runtime_net_sta_config_valid(void);
bool runtime_net_sta_connected(void);
bool runtime_net_sta_connecting(void);
bool runtime_net_sta_retry_exhausted(void);
bool runtime_net_lan_started(void);
bool runtime_net_lan_link_up(void);
bool runtime_net_lan_has_ip(void);
bool runtime_net_time_synced(void);
bool runtime_net_ntp_server_running(void);

IPAddress runtime_net_ip(void);
IPAddress runtime_net_gateway(void);
IPAddress runtime_net_subnet(void);
IPAddress runtime_net_dns1(void);
IPAddress runtime_net_dns2(void);
IPAddress runtime_net_sta_ip(void);
IPAddress runtime_net_ap_ip(void);
IPAddress runtime_net_lan_ip(void);
IPAddress runtime_net_lan_subnet(void);

const char *runtime_net_mac(void);
const char *runtime_net_ssid(void);
const char *runtime_net_ap_ssid(void);
const char *runtime_net_time_status_text(void);
int32_t runtime_net_rssi(void);
uint8_t runtime_net_ap_client_count(void);
uint8_t runtime_net_sta_retry_count(void);
uint8_t runtime_net_sta_retry_limit(void);
const char *runtime_net_last_fail_reason(void);
uint32_t runtime_net_ntp_request_count(void);
uint32_t runtime_net_ntp_last_served_ms(void);
bool runtime_net_ntp_lan_only(void);
bool runtime_net_ntp_subnet_restriction_active(void);
bool runtime_net_ntp_rate_limit_active(void);
uint8_t runtime_net_ntp_rate_limit_per_ip(void);
uint8_t runtime_net_ntp_rate_limit_global(void);
uint32_t runtime_net_ntp_rate_limit_drop_count(void);
uint32_t runtime_net_ntp_policy_drop_count(void);

bool runtime_net_scope_probe_available(void);
bool runtime_net_scope_reachable(void);
uint32_t runtime_net_scope_last_check_ms(void);
uint32_t runtime_net_scope_last_success_ms(void);

#endif /* ESP_RUNTIME_NET_H */
