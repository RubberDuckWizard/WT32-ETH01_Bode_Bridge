#ifndef ESP_SCOPE_HTTP_PROXY_H
#define ESP_SCOPE_HTTP_PROXY_H

#include <stdint.h>

struct ScopeHttpProxyStats {
    bool supported;
    bool enabled;
    bool listening;
    uint16_t listen_port;
    uint8_t active_connections;
    uint32_t accepted_connections;
    uint32_t completed_connections;
    uint32_t failed_connects;
    uint32_t dropped_connections;
    uint32_t total_client_to_scope_bytes;
    uint32_t total_scope_to_client_bytes;
    bool detected_absolute_location;
    bool detected_absolute_scope_url;
    char last_error[48];
};

void scope_http_proxy_begin();
void scope_http_proxy_poll();
bool scope_http_proxy_is_supported();
bool scope_http_proxy_is_enabled();
bool scope_http_proxy_is_listening();
uint16_t scope_http_proxy_listen_port();
const ScopeHttpProxyStats *scope_http_proxy_get_stats();

#endif /* ESP_SCOPE_HTTP_PROXY_H */