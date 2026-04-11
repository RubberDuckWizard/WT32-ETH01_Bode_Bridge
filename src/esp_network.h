#ifndef ESP_NETWORK_H
#define ESP_NETWORK_H

#include <WiFi.h>
#include "esp_config.h"

#define NET_PROTO_EVENT_COUNT  16
#define NET_PROTO_EVENT_LEN    96
#define NET_PROTO_DEVICE_LEN   32

void net_begin(void);
void net_poll(void);
bool net_is_running(void);

typedef struct {
    uint32_t udp_getport_count;
    uint32_t tcp_getport_count;
    uint32_t getport_zero_reply_count;
    uint32_t create_link_count;
    uint32_t device_write_count;
    uint32_t device_read_count;
    uint32_t destroy_link_count;
    uint32_t session_accept_count;
    uint32_t session_end_count;
    uint32_t session_drop_count;
    uint16_t current_vxi_port;
    uint16_t last_getport_reply;
    uint32_t last_write_len;
    uint32_t last_read_len;
    uint32_t last_read_declared_len;
    uint32_t unknown_proc_count;
    uint32_t malformed_packet_count;
    bool     last_read_fixed_id;
    bool     session_active;
    IPAddress active_client_ip;
    uint16_t active_client_port;
    char     last_event[80];
#if ENABLE_PROTOCOL_DIAG
    char     last_rpc_transport[4];
    bool     last_getport_valid;
    uint32_t last_map_prog;
    uint32_t last_map_vers;
    uint32_t last_map_proto;
    bool     last_vxi_valid;
    uint32_t last_vxi_program;
    uint32_t last_vxi_version;
    uint32_t last_vxi_procedure;
    uint32_t last_vxi_xid;
    bool     last_create_link_valid;
    uint32_t last_create_client_id;
    uint8_t  last_create_lock_device;
    uint32_t last_create_lock_timeout;
    char     last_create_device[NET_PROTO_DEVICE_LEN];
    char     recent_events[NET_PROTO_EVENT_COUNT][NET_PROTO_EVENT_LEN];
    uint8_t  recent_event_head;
    uint8_t  recent_event_count;
#endif
} NetStats;

const NetStats *net_get_stats(void);
void net_clear_protocol_diag(void);

#endif /* ESP_NETWORK_H */
