#ifndef ESP_WEBCONFIG_H
#define ESP_WEBCONFIG_H

#include <stdint.h>

struct WebUiServerStats {
    bool listening;
    uint16_t listen_port;
    uint8_t active_clients;
    uint8_t max_clients;
    uint32_t accepted_connections;
    uint32_t completed_connections;
    uint32_t rejected_connections;
    char last_error[48];
};

void webconfig_begin(void);
void webconfig_poll(void);
const WebUiServerStats *webconfig_get_stats(void);

#endif /* ESP_WEBCONFIG_H */
