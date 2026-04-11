#ifndef ESP_SCOPE_VNC_PROXY_H
#define ESP_SCOPE_VNC_PROXY_H

#include <stdint.h>

struct ScopeVncProxyStats {
    bool supported;
    bool enabled;
    bool listening;
    uint16_t listen_port;
    uint16_t target_port;
    uint8_t active_connections;
    uint8_t max_connections;
    uint32_t accepted_connections;
    uint32_t completed_connections;
    uint32_t failed_connects;
    uint32_t dropped_connections;
    uint32_t total_client_to_scope_bytes;
    uint32_t total_scope_to_client_bytes;
    bool detected_websocket_upgrade;
    bool detected_websockify_path;
    char last_error[48];
};

void scope_vnc_proxy_begin();
void scope_vnc_proxy_poll();
bool scope_vnc_proxy_is_supported();
bool scope_vnc_proxy_is_enabled();
bool scope_vnc_proxy_is_listening();
uint16_t scope_vnc_proxy_listen_port();
const ScopeVncProxyStats *scope_vnc_proxy_get_stats();

#endif /* ESP_SCOPE_VNC_PROXY_H */