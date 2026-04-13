#ifndef ESP_PERSIST_H
#define ESP_PERSIST_H

#include <stdint.h>

#define CONFIG_VERSION      10
#define CONFIG_EEPROM_ADDR  0
#define CONFIG_EEPROM_SIZE  256

struct EspConfig {
    uint8_t  version;
    uint8_t  use_dhcp;
    uint8_t  recovery_ap_enable;
    uint8_t  scope_http_proxy_enable;
    uint8_t  max_web_ui_clients;
    uint8_t  max_scope_http_proxy_clients;
    uint8_t  max_scope_vnc_proxy_clients;
    uint8_t  ip[4];
    uint8_t  mask[4];
    uint8_t  gw[4];
    uint8_t  dns[4];
    uint8_t  dns2[4];
    uint8_t  lan_ip[4];
    uint8_t  lan_mask[4];
    uint8_t  scope_ip[4];
    char     device_hostname[33];
    char     wifi_ssid[33];
    char     wifi_password[65];
    char     ap_ssid[33];
    char     ap_password[65];
    char     ntp_server[33];
    char     friendly_name[25];
    char     idn_response_name[17];
    char     awg_serial_mode[5];
    uint16_t scope_port;
    uint16_t scope_connect_timeout_ms;
    uint16_t scope_probe_interval_ms;
    uint16_t vxi_session_timeout_ms;
    uint16_t awg_serial_timeout_ms;
    uint16_t auto_output_off_timeout_ms;
    uint32_t awg_baud;
    uint8_t  awg_firmware_family;
    uint8_t  reserved1[1];
};

extern EspConfig g_config;

bool loadConfig();
bool saveConfig();
void resetConfigToDefaults();
bool config_store_was_valid();
bool config_store_needs_commit();
bool config_last_save_wrote();
bool config_current_is_valid();
bool config_has_valid_sta_settings();
bool config_loaded_from_nvs_ok();
bool config_running_from_ram_recovery();
bool config_save_required();
const char *config_last_error_reason();
void config_mark_running_from_ram_recovery(const char *reason, bool save_required);

void config_init();
bool config_load();
void config_save();
void config_reset_defaults();

#endif /* ESP_PERSIST_H */
