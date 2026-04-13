#include <EEPROM.h>
#include <Preferences.h>
#include <ctype.h>
#include <string.h>

#include "esp_persist.h"
#include "esp_config.h"
#include "esp_fy6900.h"

EspConfig g_config;
static bool s_store_was_valid = false;
static EspConfig s_persisted_config;
static bool s_persisted_config_known = false;
static bool s_store_needs_commit = false;
static bool s_last_save_wrote = false;
static bool s_config_loaded_from_nvs_ok = false;
static bool s_config_running_from_ram_recovery = false;
static char s_last_config_error_reason[48] = "none";

namespace {

Preferences s_prefs;

const char *const kPrefsNamespace = "espbode";
const uint8_t kIpv4Len = 4u;

static const uint8_t kDefStaIp[] = { DEF_STA_IP };
static const uint8_t kDefStaMask[] = { DEF_STA_MASK };
static const uint8_t kDefStaGw[] = { DEF_STA_GW };
static const uint8_t kDefStaDns[] = { DEF_STA_DNS };
static const uint8_t kDefStaDns2[] = { DEF_STA_DNS2 };
static const uint8_t kDefLanIp[] = { DEF_LAN_IP };
static const uint8_t kDefLanMask[] = { DEF_LAN_MASK };
static const uint8_t kDefScopeIp[] = { DEF_SCOPE_IP };

struct LegacyConfig {
    uint16_t magic;
    uint8_t  version;
    uint8_t  use_dhcp;
    uint8_t  ip[4];
    uint8_t  mask[4];
    uint8_t  gw[4];
    uint8_t  dns[4];
    char     device_hostname[25];
    char     friendly_name[25];
    char     idn_response_name[17];
    uint32_t awg_baud;
    uint8_t  awg_firmware_family;
    uint16_t crc;
};

enum NormalizeFlags : uint32_t {
    kNormalizeNone = 0u,
    kNormalizeVersion = 1u << 0,
    kNormalizeHostname = 1u << 1,
    kNormalizeApSsid = 1u << 2,
    kNormalizeApPassword = 1u << 3,
    kNormalizeNtpServer = 1u << 4,
    kNormalizeFriendlyName = 1u << 5,
    kNormalizeIdnName = 1u << 6,
    kNormalizeAwgSerialMode = 1u << 7,
    kNormalizeAwgBaud = 1u << 8,
    kNormalizeAwgFamily = 1u << 9,
    kNormalizeScopePort = 1u << 10,
    kNormalizeScopeTimeout = 1u << 11,
    kNormalizeScopeProbe = 1u << 12,
    kNormalizeVxiTimeout = 1u << 13,
    kNormalizeAwgTimeout = 1u << 14,
    kNormalizeAutoOffTimeout = 1u << 15,
    kNormalizeRecoveryApEnable = 1u << 16,
    kNormalizeScopeProxyEnable = 1u << 17,
    kNormalizeWebLimit = 1u << 18,
    kNormalizeProxyLimit = 1u << 19,
    kNormalizeVncLimit = 1u << 20,
    kNormalizeWiFiTuple = 1u << 21,
    kNormalizeDns2 = 1u << 22,
    kNormalizeLanTuple = 1u << 23,
    kNormalizeScopeIp = 1u << 24,
    kNormalizeWiFiLanConflict = 1u << 25,
    kNormalizeWiFiSsid = 1u << 26,
    kNormalizeWiFiPassword = 1u << 27,
    kNormalizeUseDhcp = 1u << 28
};

static bool recovery_ap_password_can_be_open()
{
    return FW_FORCE_RECOVERY_AP != 0;
}

static void copy_text(char *dst, size_t dst_len, const char *src)
{
    if (dst_len == 0) return;
    if (src == NULL) src = "";
    strncpy(dst, src, dst_len - 1);
    dst[dst_len - 1] = '\0';
}

static void copy_ip(uint8_t *dst, const uint8_t *src)
{
    memcpy(dst, src, kIpv4Len);
}

static bool config_equals(const EspConfig &left, const EspConfig &right)
{
    return memcmp(&left, &right, sizeof(EspConfig)) == 0;
}

static void set_config_error_reason(const char *reason)
{
    if (reason == NULL || reason[0] == '\0') {
        reason = "none";
    }
    strncpy(s_last_config_error_reason, reason, sizeof(s_last_config_error_reason) - 1u);
    s_last_config_error_reason[sizeof(s_last_config_error_reason) - 1u] = '\0';
}

static void clear_config_error_reason(void)
{
    set_config_error_reason("none");
}

static void cache_persisted_config(const EspConfig &cfg)
{
    s_persisted_config = cfg;
    s_persisted_config_known = true;
}

static void remember_persisted_config(const EspConfig &cfg)
{
    cache_persisted_config(cfg);
    s_store_was_valid = true;
    s_config_loaded_from_nvs_ok = true;
    s_config_running_from_ram_recovery = false;
    s_store_needs_commit = false;
    clear_config_error_reason();
}

static void mark_running_from_ram_recovery(const char *reason, bool save_required)
{
    s_store_was_valid = false;
    s_config_loaded_from_nvs_ok = false;
    s_config_running_from_ram_recovery = true;
    s_store_needs_commit = save_required;
    set_config_error_reason(reason);
}

static uint32_t ip_to_u32(const uint8_t *ip4)
{
    return ((uint32_t)ip4[0] << 24)
        | ((uint32_t)ip4[1] << 16)
        | ((uint32_t)ip4[2] << 8)
        | (uint32_t)ip4[3];
}

static void u32_to_ip(uint32_t value, uint8_t *ip4)
{
    ip4[0] = (uint8_t)(value >> 24);
    ip4[1] = (uint8_t)(value >> 16);
    ip4[2] = (uint8_t)(value >> 8);
    ip4[3] = (uint8_t)value;
}

static bool ip_is_zero(const uint8_t *ip4)
{
    return ip4[0] == 0 && ip4[1] == 0 && ip4[2] == 0 && ip4[3] == 0;
}

static bool subnet_is_valid(const uint8_t *mask)
{
    uint32_t value = ip_to_u32(mask);
    bool zero_seen = false;

    if (value == 0u || value == 0xFFFFFFFFu) return false;

    for (int bit = 31; bit >= 0; --bit) {
        bool one = ((value >> bit) & 1u) != 0u;
        if (!one) {
            zero_seen = true;
        } else if (zero_seen) {
            return false;
        }
    }
    return true;
}

static bool host_is_valid_for_mask(const uint8_t *ip4, const uint8_t *mask)
{
    uint32_t ip = ip_to_u32(ip4);
    uint32_t host_mask = ~ip_to_u32(mask);
    uint32_t host_bits = ip & host_mask;

    if (!subnet_is_valid(mask)) return false;
    return host_bits != 0u && host_bits != host_mask;
}

static bool host_bits_are_valid(uint32_t host_bits, uint32_t host_mask)
{
    return host_bits != 0u && host_bits != host_mask;
}

static uint32_t pick_scope_host_bits(uint32_t current_scope_u32, uint32_t lan_u32, uint32_t mask_u32)
{
    uint32_t host_mask = ~mask_u32;
    uint32_t lan_host_bits = lan_u32 & host_mask;
    uint32_t candidates[] = {
        current_scope_u32 & host_mask,
        ip_to_u32(kDefScopeIp) & host_mask,
        ((ip_to_u32(kDefScopeIp) & host_mask) + 1u) & host_mask,
        ((lan_host_bits + 1u) & host_mask),
        lan_host_bits > 1u ? lan_host_bits - 1u : 1u,
        1u,
        2u,
        host_mask > 1u ? host_mask - 1u : 0u
    };

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        uint32_t host_bits = candidates[i];
        if (!host_bits_are_valid(host_bits, host_mask)) continue;
        if (host_bits == lan_host_bits) continue;
        return host_bits;
    }

    return 1u;
}

static void normalize_scope_ip_for_lan(uint8_t *scope_ip, const uint8_t *lan_ip, const uint8_t *lan_mask)
{
    uint32_t mask_u32 = ip_to_u32(lan_mask);
    uint32_t lan_u32 = ip_to_u32(lan_ip);
    uint32_t network_bits = lan_u32 & mask_u32;
    uint32_t host_bits = pick_scope_host_bits(ip_to_u32(scope_ip), lan_u32, mask_u32);

    u32_to_ip(network_bits | host_bits, scope_ip);
}

static bool nonzero_ip_is_valid(const uint8_t *ip4)
{
    return !ip_is_zero(ip4) && ip4[0] != 255u && ip4[3] != 255u;
}

static bool same_subnet(const uint8_t *left, const uint8_t *right, const uint8_t *mask)
{
    uint32_t mask_u32 = ip_to_u32(mask);
    return (ip_to_u32(left) & mask_u32) == (ip_to_u32(right) & mask_u32);
}

static bool safe_ascii_text(const char *text, size_t max_len, bool allow_space)
{
    size_t len;

    if (text == NULL) return false;
    len = strlen(text);
    if (len == 0 || len > max_len) return false;

    for (size_t i = 0; i < len; ++i) {
        unsigned char ch = (unsigned char)text[i];
        if (ch < 32u || ch > 126u) return false;
        if (!allow_space && ch == ' ') return false;
        if (ch == '\'' || ch == '"' || ch == '<' || ch == '>' || ch == '&') return false;
    }
    return true;
}

static bool hostname_is_valid(const char *text)
{
    size_t len;

    if (text == NULL) return false;
    len = strlen(text);
    if (len == 0 || len > 32u) return false;
    if (text[0] == '-' || text[len - 1] == '-') return false;

    for (size_t i = 0; i < len; ++i) {
        unsigned char ch = (unsigned char)text[i];
        if (!isalnum(ch) && ch != '-') return false;
    }
    return true;
}

static bool recovery_ap_ssid_is_valid(const char *text)
{
    return safe_ascii_text(text, 32u, true);
}

static bool recovery_ap_password_is_valid(const char *text)
{
    size_t len;

    if (text == NULL) return false;
    len = strlen(text);
    if (len == 0u) return recovery_ap_password_can_be_open();
    if (len < RECOVERY_AP_MIN_PASSWORD_LEN || len > 63u) return false;
    return safe_ascii_text(text, 63u, true);
}

static bool idn_name_is_valid(const char *text)
{
    size_t len;

    if (text == NULL) return false;
    len = strlen(text);
    if (len == 0 || len > sizeof(g_config.idn_response_name) - 1) return false;

    for (size_t i = 0; i < len; ++i) {
        unsigned char ch = (unsigned char)text[i];
        if (!isalnum(ch) && ch != '-' && ch != '_' && ch != '.') return false;
    }
    return true;
}

static uint8_t normalize_web_service_client_limit(uint8_t value)
{
    if (value < MIN_WEB_SERVICE_CLIENTS) {
        return MIN_WEB_SERVICE_CLIENTS;
    }
    if (value > MAX_WEB_SERVICE_CLIENTS) {
        return MAX_WEB_SERVICE_CLIENTS;
    }
    return value;
}

static void set_default_config(EspConfig *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->version = CONFIG_VERSION;
    cfg->use_dhcp = DEF_USE_DHCP;
    cfg->recovery_ap_enable = DEF_RECOVERY_AP_ENABLE;
    cfg->scope_http_proxy_enable = DEF_SCOPE_HTTP_PROXY_ENABLE;
    cfg->max_web_ui_clients = DEF_WEB_UI_MAX_CLIENTS;
    cfg->max_scope_http_proxy_clients = DEF_SCOPE_HTTP_PROXY_MAX_CLIENTS;
    cfg->max_scope_vnc_proxy_clients = DEF_SCOPE_VNC_PROXY_MAX_CLIENTS;

    copy_ip(cfg->ip, kDefStaIp);
    copy_ip(cfg->mask, kDefStaMask);
    copy_ip(cfg->gw, kDefStaGw);
    copy_ip(cfg->dns, kDefStaDns);
    copy_ip(cfg->dns2, kDefStaDns2);
    copy_ip(cfg->lan_ip, kDefLanIp);
    copy_ip(cfg->lan_mask, kDefLanMask);
    copy_ip(cfg->scope_ip, kDefScopeIp);

    copy_text(cfg->device_hostname, sizeof(cfg->device_hostname), DEF_DEVICE_HOSTNAME);
    copy_text(cfg->wifi_ssid, sizeof(cfg->wifi_ssid), DEF_WIFI_SSID);
    copy_text(cfg->wifi_password, sizeof(cfg->wifi_password), DEF_WIFI_PASSWORD);
    copy_text(cfg->ap_ssid, sizeof(cfg->ap_ssid), DEF_RECOVERY_AP_SSID);
    copy_text(cfg->ap_password, sizeof(cfg->ap_password), DEF_RECOVERY_AP_PASSWORD);
    copy_text(cfg->ntp_server, sizeof(cfg->ntp_server), DEF_NTP_SERVER);
    copy_text(cfg->friendly_name, sizeof(cfg->friendly_name), DEF_FRIENDLY_NAME);
    copy_text(cfg->idn_response_name, sizeof(cfg->idn_response_name), DEF_IDN_RESPONSE_NAME);
    copy_text(cfg->awg_serial_mode, sizeof(cfg->awg_serial_mode), DEF_AWG_SERIAL_MODE);

    cfg->scope_port = DEF_SCOPE_PORT;
    cfg->scope_connect_timeout_ms = DEF_SCOPE_CONNECT_TIMEOUT_MS;
    cfg->scope_probe_interval_ms = DEF_SCOPE_PROBE_INTERVAL_MS;
    cfg->vxi_session_timeout_ms = DEF_VXI_SESSION_TIMEOUT_MS;
    cfg->awg_serial_timeout_ms = DEF_AWG_SERIAL_TIMEOUT_MS;
    cfg->auto_output_off_timeout_ms = DEF_AUTO_OUTPUT_OFF_TIMEOUT_MS;
    cfg->awg_baud = AWG_BAUD_RATE;
    cfg->awg_firmware_family = DEF_AWG_FW_FAMILY;
}

static uint32_t normalize_config(EspConfig *cfg)
{
    uint32_t flags = kNormalizeNone;

    if (cfg->version != CONFIG_VERSION) {
        flags |= kNormalizeVersion;
    }
    cfg->version = CONFIG_VERSION;
    cfg->device_hostname[sizeof(cfg->device_hostname) - 1] = '\0';
    cfg->wifi_ssid[sizeof(cfg->wifi_ssid) - 1] = '\0';
    cfg->wifi_password[sizeof(cfg->wifi_password) - 1] = '\0';
    cfg->ap_ssid[sizeof(cfg->ap_ssid) - 1] = '\0';
    cfg->ap_password[sizeof(cfg->ap_password) - 1] = '\0';
    cfg->ntp_server[sizeof(cfg->ntp_server) - 1] = '\0';
    cfg->friendly_name[sizeof(cfg->friendly_name) - 1] = '\0';
    cfg->idn_response_name[sizeof(cfg->idn_response_name) - 1] = '\0';
    cfg->awg_serial_mode[sizeof(cfg->awg_serial_mode) - 1] = '\0';

    if (cfg->use_dhcp > 1u) {
        cfg->use_dhcp = DEF_USE_DHCP;
        flags |= kNormalizeUseDhcp;
    }

    if (!hostname_is_valid(cfg->device_hostname)) {
        copy_text(cfg->device_hostname, sizeof(cfg->device_hostname), DEF_DEVICE_HOSTNAME);
        flags |= kNormalizeHostname;
    }
    if (!recovery_ap_ssid_is_valid(cfg->ap_ssid)) {
        copy_text(cfg->ap_ssid, sizeof(cfg->ap_ssid), DEF_RECOVERY_AP_SSID);
        flags |= kNormalizeApSsid;
    }
    if (!recovery_ap_password_is_valid(cfg->ap_password)) {
        copy_text(cfg->ap_password, sizeof(cfg->ap_password), DEF_RECOVERY_AP_PASSWORD);
        flags |= kNormalizeApPassword;
    }
    if (!idn_name_is_valid(cfg->ntp_server)) {
        copy_text(cfg->ntp_server, sizeof(cfg->ntp_server), DEF_NTP_SERVER);
        flags |= kNormalizeNtpServer;
    }
    if (!safe_ascii_text(cfg->friendly_name, sizeof(cfg->friendly_name) - 1, true)) {
        copy_text(cfg->friendly_name, sizeof(cfg->friendly_name), DEF_FRIENDLY_NAME);
        flags |= kNormalizeFriendlyName;
    }
    if (!idn_name_is_valid(cfg->idn_response_name)) {
        copy_text(cfg->idn_response_name, sizeof(cfg->idn_response_name), DEF_IDN_RESPONSE_NAME);
        flags |= kNormalizeIdnName;
    }
    if (!fy_is_supported_serial_mode(cfg->awg_serial_mode)) {
        copy_text(cfg->awg_serial_mode, sizeof(cfg->awg_serial_mode), DEF_AWG_SERIAL_MODE);
        flags |= kNormalizeAwgSerialMode;
    }
    if (!fy_is_supported_baud(cfg->awg_baud)) {
        cfg->awg_baud = AWG_BAUD_RATE;
        flags |= kNormalizeAwgBaud;
    }
    if (!fy_is_supported_firmware_family(cfg->awg_firmware_family)) {
        cfg->awg_firmware_family = DEF_AWG_FW_FAMILY;
        flags |= kNormalizeAwgFamily;
    }
    if (cfg->scope_port == 0u) {
        cfg->scope_port = DEF_SCOPE_PORT;
        flags |= kNormalizeScopePort;
    }
    if (cfg->scope_connect_timeout_ms < MIN_SCOPE_TIMEOUT_MS || cfg->scope_connect_timeout_ms > MAX_SCOPE_TIMEOUT_MS) {
        cfg->scope_connect_timeout_ms = DEF_SCOPE_CONNECT_TIMEOUT_MS;
        flags |= kNormalizeScopeTimeout;
    }
    if (cfg->scope_probe_interval_ms < MIN_SCOPE_INTERVAL_MS || cfg->scope_probe_interval_ms > MAX_SCOPE_INTERVAL_MS) {
        cfg->scope_probe_interval_ms = DEF_SCOPE_PROBE_INTERVAL_MS;
        flags |= kNormalizeScopeProbe;
    }
    if (cfg->vxi_session_timeout_ms < MIN_VXI_TIMEOUT_MS || cfg->vxi_session_timeout_ms > MAX_VXI_TIMEOUT_MS) {
        cfg->vxi_session_timeout_ms = DEF_VXI_SESSION_TIMEOUT_MS;
        flags |= kNormalizeVxiTimeout;
    }
    if (cfg->awg_serial_timeout_ms < MIN_AWG_TIMEOUT_MS || cfg->awg_serial_timeout_ms > MAX_AWG_TIMEOUT_MS) {
        cfg->awg_serial_timeout_ms = DEF_AWG_SERIAL_TIMEOUT_MS;
        flags |= kNormalizeAwgTimeout;
    }
    if (cfg->auto_output_off_timeout_ms < MIN_AUTO_OFF_TIMEOUT_MS || cfg->auto_output_off_timeout_ms > MAX_AUTO_OFF_TIMEOUT_MS) {
        cfg->auto_output_off_timeout_ms = DEF_AUTO_OUTPUT_OFF_TIMEOUT_MS;
        flags |= kNormalizeAutoOffTimeout;
    }
    if (cfg->recovery_ap_enable > 1u) {
        cfg->recovery_ap_enable = DEF_RECOVERY_AP_ENABLE;
        flags |= kNormalizeRecoveryApEnable;
    }
    if (cfg->scope_http_proxy_enable > 1u) {
        cfg->scope_http_proxy_enable = DEF_SCOPE_HTTP_PROXY_ENABLE;
        flags |= kNormalizeScopeProxyEnable;
    }
    if (cfg->max_web_ui_clients != normalize_web_service_client_limit(cfg->max_web_ui_clients)) {
        cfg->max_web_ui_clients = normalize_web_service_client_limit(cfg->max_web_ui_clients);
        flags |= kNormalizeWebLimit;
    }
    if (cfg->max_scope_http_proxy_clients != normalize_web_service_client_limit(cfg->max_scope_http_proxy_clients)) {
        cfg->max_scope_http_proxy_clients = normalize_web_service_client_limit(cfg->max_scope_http_proxy_clients);
        flags |= kNormalizeProxyLimit;
    }
    if (cfg->max_scope_vnc_proxy_clients != normalize_web_service_client_limit(cfg->max_scope_vnc_proxy_clients)) {
        cfg->max_scope_vnc_proxy_clients = normalize_web_service_client_limit(cfg->max_scope_vnc_proxy_clients);
        flags |= kNormalizeVncLimit;
    }
#if ENABLE_ETH_RUNTIME
    if (!subnet_is_valid(cfg->lan_mask) || !host_is_valid_for_mask(cfg->lan_ip, cfg->lan_mask)) {
        copy_ip(cfg->lan_ip, kDefLanIp);
        copy_ip(cfg->lan_mask, kDefLanMask);
        flags |= kNormalizeLanTuple;
    }
    if (!nonzero_ip_is_valid(cfg->scope_ip)
            || !same_subnet(cfg->scope_ip, cfg->lan_ip, cfg->lan_mask)
            || ip_to_u32(cfg->scope_ip) == ip_to_u32(cfg->lan_ip)) {
        normalize_scope_ip_for_lan(cfg->scope_ip, cfg->lan_ip, cfg->lan_mask);
        flags |= kNormalizeScopeIp;
    }
#else
    if (!nonzero_ip_is_valid(cfg->scope_ip)) {
        copy_ip(cfg->scope_ip, kDefScopeIp);
        flags |= kNormalizeScopeIp;
    }
#endif

    if (!cfg->use_dhcp) {
        if (!subnet_is_valid(cfg->mask)
                || !host_is_valid_for_mask(cfg->ip, cfg->mask)
                || !host_is_valid_for_mask(cfg->gw, cfg->mask)
                || !same_subnet(cfg->ip, cfg->gw, cfg->mask)
                || !nonzero_ip_is_valid(cfg->dns)) {
            cfg->use_dhcp = 1u;
            flags |= kNormalizeWiFiTuple | kNormalizeUseDhcp;
        } else if (!ip_is_zero(cfg->dns2) && !nonzero_ip_is_valid(cfg->dns2)) {
            copy_ip(cfg->dns2, kDefStaDns2);
            flags |= kNormalizeDns2;
        }
#if ENABLE_ETH_RUNTIME
        if (!cfg->use_dhcp && same_subnet(cfg->ip, cfg->lan_ip, cfg->lan_mask)) {
            cfg->use_dhcp = 1u;
            flags |= kNormalizeWiFiLanConflict | kNormalizeUseDhcp;
        }
#endif
    }

    if (cfg->wifi_ssid[0] != '\0' && !safe_ascii_text(cfg->wifi_ssid, sizeof(cfg->wifi_ssid) - 1, true)) {
        cfg->wifi_ssid[0] = '\0';
        cfg->wifi_password[0] = '\0';
        flags |= kNormalizeWiFiSsid | kNormalizeWiFiPassword;
    }
    if (cfg->wifi_ssid[0] == '\0' && cfg->wifi_password[0] != '\0') {
        cfg->wifi_password[0] = '\0';
        flags |= kNormalizeWiFiPassword;
    }
    if (cfg->wifi_password[0] != '\0') {
        size_t pass_len = strlen(cfg->wifi_password);
        if (pass_len < 8u || pass_len > 63u || !safe_ascii_text(cfg->wifi_password, sizeof(cfg->wifi_password) - 1, true)) {
            cfg->wifi_password[0] = '\0';
            flags |= kNormalizeWiFiPassword;
        }
    }

    return flags;
}

static uint16_t crc16_ccitt(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint16_t crc = 0xFFFFu;

    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)p[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                                  : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static bool read_ip_pref(const char *key, uint8_t *dst)
{
    return s_prefs.getBytes(key, dst, kIpv4Len) == kIpv4Len;
}

static void write_ip_pref(const char *key, const uint8_t *src)
{
    s_prefs.putBytes(key, src, kIpv4Len);
}

static bool prefs_has_required_keys()
{
    static const char *const kRequiredScalarKeys[] = {
        "version", "dhcp", "recovery_ap", "scope_proxy",
        "web_max", "proxy_max", "vnc_max",
        "scope_port", "scope_conn_to", "scope_probe_ms", "vxi_to",
        "fy_to", "auto_off", "fy_baud", "fy_family"
    };
    static const char *const kRequiredIpKeys[] = {
        "local_ip", "subnet", "gateway", "dns1", "dns2",
        "lan_ip", "lan_mask", "scope_ip"
    };
    static const char *const kRequiredTextKeys[] = {
        "hostname", "ssid", "password", "ap_ssid", "ap_password",
        "ntp_host", "friendly", "idn_name", "fy_mode"
    };

    for (size_t i = 0; i < sizeof(kRequiredScalarKeys) / sizeof(kRequiredScalarKeys[0]); ++i) {
        if (!s_prefs.isKey(kRequiredScalarKeys[i])) {
            return false;
        }
    }
    for (size_t i = 0; i < sizeof(kRequiredIpKeys) / sizeof(kRequiredIpKeys[0]); ++i) {
        if (!s_prefs.isKey(kRequiredIpKeys[i])) {
            return false;
        }
    }
    for (size_t i = 0; i < sizeof(kRequiredTextKeys) / sizeof(kRequiredTextKeys[0]); ++i) {
        if (!s_prefs.isKey(kRequiredTextKeys[i])) {
            return false;
        }
    }
    return true;
}

static const char *classify_normalize_reason(uint32_t normalize_flags)
{
    if ((normalize_flags & kNormalizeVersion) != 0u) {
        return "incompatible_config_in_nvs";
    }
    return "invalid_or_corrupted_config_in_nvs";
}

static bool load_legacy_eeprom_to_ram()
{
    LegacyConfig legacy;

    EEPROM.begin(CONFIG_EEPROM_SIZE);
    EEPROM.get(CONFIG_EEPROM_ADDR, legacy);

    if (legacy.magic != 0xB0DEu) return false;
    if (legacy.version != 4u) return false;
    if (crc16_ccitt(&legacy, sizeof(legacy) - sizeof(legacy.crc)) != legacy.crc) return false;

    set_default_config(&g_config);
    g_config.use_dhcp = legacy.use_dhcp ? 1u : 0u;
    copy_ip(g_config.ip, legacy.ip);
    copy_ip(g_config.mask, legacy.mask);
    copy_ip(g_config.gw, legacy.gw);
    copy_ip(g_config.dns, legacy.dns);
    copy_ip(g_config.dns2, legacy.dns);
    copy_text(g_config.device_hostname, sizeof(g_config.device_hostname), legacy.device_hostname);
    copy_text(g_config.friendly_name, sizeof(g_config.friendly_name), legacy.friendly_name);
    copy_text(g_config.idn_response_name, sizeof(g_config.idn_response_name), legacy.idn_response_name);
    g_config.awg_baud = legacy.awg_baud;
    g_config.awg_firmware_family = legacy.awg_firmware_family;

    (void)normalize_config(&g_config);
    return true;
}

static void load_from_preferences()
{
    set_default_config(&g_config);

    g_config.version = s_prefs.getUChar("version", CONFIG_VERSION);
    g_config.use_dhcp = s_prefs.getBool("dhcp", DEF_USE_DHCP) ? 1u : 0u;
    g_config.recovery_ap_enable = s_prefs.getBool("recovery_ap", DEF_RECOVERY_AP_ENABLE) ? 1u : 0u;
    g_config.scope_http_proxy_enable = s_prefs.getBool("scope_proxy", DEF_SCOPE_HTTP_PROXY_ENABLE) ? 1u : 0u;
    g_config.max_web_ui_clients = s_prefs.getUChar("web_max", DEF_WEB_UI_MAX_CLIENTS);
    g_config.max_scope_http_proxy_clients = s_prefs.getUChar("proxy_max", DEF_SCOPE_HTTP_PROXY_MAX_CLIENTS);
    g_config.max_scope_vnc_proxy_clients = s_prefs.getUChar("vnc_max", DEF_SCOPE_VNC_PROXY_MAX_CLIENTS);
    g_config.scope_port = s_prefs.getUShort("scope_port", DEF_SCOPE_PORT);
    g_config.scope_connect_timeout_ms = s_prefs.getUShort("scope_conn_to", DEF_SCOPE_CONNECT_TIMEOUT_MS);
    g_config.scope_probe_interval_ms = s_prefs.getUShort("scope_probe_ms", DEF_SCOPE_PROBE_INTERVAL_MS);
    g_config.vxi_session_timeout_ms = s_prefs.getUShort("vxi_to", DEF_VXI_SESSION_TIMEOUT_MS);
    g_config.awg_serial_timeout_ms = s_prefs.getUShort("fy_to", DEF_AWG_SERIAL_TIMEOUT_MS);
    g_config.auto_output_off_timeout_ms = s_prefs.getUShort("auto_off", DEF_AUTO_OUTPUT_OFF_TIMEOUT_MS);
    g_config.awg_baud = s_prefs.getUInt("fy_baud", AWG_BAUD_RATE);
    g_config.awg_firmware_family = s_prefs.getUChar("fy_family", DEF_AWG_FW_FAMILY);

    (void)read_ip_pref("local_ip", g_config.ip);
    (void)read_ip_pref("subnet", g_config.mask);
    (void)read_ip_pref("gateway", g_config.gw);
    (void)read_ip_pref("dns1", g_config.dns);
    (void)read_ip_pref("dns2", g_config.dns2);
    (void)read_ip_pref("lan_ip", g_config.lan_ip);
    (void)read_ip_pref("lan_mask", g_config.lan_mask);
    (void)read_ip_pref("scope_ip", g_config.scope_ip);

    (void)s_prefs.getString("hostname", g_config.device_hostname, sizeof(g_config.device_hostname));
    (void)s_prefs.getString("ssid", g_config.wifi_ssid, sizeof(g_config.wifi_ssid));
    (void)s_prefs.getString("password", g_config.wifi_password, sizeof(g_config.wifi_password));
    (void)s_prefs.getString("ap_ssid", g_config.ap_ssid, sizeof(g_config.ap_ssid));
    (void)s_prefs.getString("ap_password", g_config.ap_password, sizeof(g_config.ap_password));
    (void)s_prefs.getString("ntp_host", g_config.ntp_server, sizeof(g_config.ntp_server));
    (void)s_prefs.getString("friendly", g_config.friendly_name, sizeof(g_config.friendly_name));
    (void)s_prefs.getString("idn_name", g_config.idn_response_name, sizeof(g_config.idn_response_name));
    (void)s_prefs.getString("fy_mode", g_config.awg_serial_mode, sizeof(g_config.awg_serial_mode));
}

}  // namespace

bool loadConfig()
{
    uint32_t normalize_flags;
    bool has_required_keys = false;
    EspConfig raw_loaded_config;

    set_default_config(&g_config);
    s_store_was_valid = false;
    s_config_loaded_from_nvs_ok = false;
    s_config_running_from_ram_recovery = false;
    s_store_needs_commit = false;
    s_persisted_config_known = false;
    s_last_save_wrote = false;
    clear_config_error_reason();

    if (!s_prefs.begin(kPrefsNamespace, false)) {
        mark_running_from_ram_recovery("preferences_open_failed", true);
        return false;
    }

    if (!s_prefs.isKey("version")) {
        s_prefs.end();
        if (load_legacy_eeprom_to_ram()) {
            mark_running_from_ram_recovery("incompatible_legacy_config", true);
            return false;
        }
        set_default_config(&g_config);
        mark_running_from_ram_recovery("config_missing_in_nvs", true);
        return false;
    }

    has_required_keys = prefs_has_required_keys();
    load_from_preferences();
    raw_loaded_config = g_config;
    s_prefs.end();

    normalize_flags = normalize_config(&g_config);
    if (has_required_keys && normalize_flags == kNormalizeNone) {
        remember_persisted_config(g_config);
        return true;
    }

    cache_persisted_config(raw_loaded_config);
    if (!has_required_keys) {
        mark_running_from_ram_recovery("incomplete_config_in_nvs", true);
    } else {
        mark_running_from_ram_recovery(classify_normalize_reason(normalize_flags), true);
    }
    return false;
}

bool saveConfig()
{
    (void)normalize_config(&g_config);

    if (!s_store_needs_commit
            && s_persisted_config_known
            && config_equals(g_config, s_persisted_config)) {
        s_last_save_wrote = false;
        return true;
    }

    if (!s_prefs.begin(kPrefsNamespace, false)) {
        s_last_save_wrote = false;
        s_store_needs_commit = true;
        return false;
    }

    s_prefs.putUChar("version", CONFIG_VERSION);
    s_prefs.putBool("dhcp", g_config.use_dhcp != 0u);
    s_prefs.putBool("recovery_ap", g_config.recovery_ap_enable != 0u);
    s_prefs.putBool("scope_proxy", g_config.scope_http_proxy_enable != 0u);
    s_prefs.putUChar("web_max", g_config.max_web_ui_clients);
    s_prefs.putUChar("proxy_max", g_config.max_scope_http_proxy_clients);
    s_prefs.putUChar("vnc_max", g_config.max_scope_vnc_proxy_clients);
    s_prefs.putUShort("scope_port", g_config.scope_port);
    s_prefs.putUShort("scope_conn_to", g_config.scope_connect_timeout_ms);
    s_prefs.putUShort("scope_probe_ms", g_config.scope_probe_interval_ms);
    s_prefs.putUShort("vxi_to", g_config.vxi_session_timeout_ms);
    s_prefs.putUShort("fy_to", g_config.awg_serial_timeout_ms);
    s_prefs.putUShort("auto_off", g_config.auto_output_off_timeout_ms);
    s_prefs.putUInt("fy_baud", g_config.awg_baud);
    s_prefs.putUChar("fy_family", g_config.awg_firmware_family);

    write_ip_pref("local_ip", g_config.ip);
    write_ip_pref("subnet", g_config.mask);
    write_ip_pref("gateway", g_config.gw);
    write_ip_pref("dns1", g_config.dns);
    write_ip_pref("dns2", g_config.dns2);
    write_ip_pref("lan_ip", g_config.lan_ip);
    write_ip_pref("lan_mask", g_config.lan_mask);
    write_ip_pref("scope_ip", g_config.scope_ip);

    s_prefs.putString("hostname", g_config.device_hostname);
    s_prefs.putString("ssid", g_config.wifi_ssid);
    s_prefs.putString("password", g_config.wifi_password);
    s_prefs.putString("ap_ssid", g_config.ap_ssid);
    s_prefs.putString("ap_password", g_config.ap_password);
    s_prefs.putString("ntp_host", g_config.ntp_server);
    s_prefs.putString("friendly", g_config.friendly_name);
    s_prefs.putString("idn_name", g_config.idn_response_name);
    s_prefs.putString("fy_mode", g_config.awg_serial_mode);
    s_prefs.end();

    remember_persisted_config(g_config);
    s_last_save_wrote = true;
    return true;
}

void resetConfigToDefaults()
{
    set_default_config(&g_config);
}

bool config_store_was_valid()
{
    return s_store_was_valid;
}

bool config_store_needs_commit()
{
    return s_store_needs_commit;
}

bool config_loaded_from_nvs_ok()
{
    return s_config_loaded_from_nvs_ok;
}

bool config_running_from_ram_recovery()
{
    return s_config_running_from_ram_recovery;
}

bool config_save_required()
{
    return s_store_needs_commit;
}

const char *config_last_error_reason()
{
    return s_last_config_error_reason;
}

bool config_last_save_wrote()
{
    return s_last_save_wrote;
}

bool config_current_is_valid()
{
    EspConfig copy = g_config;
    return normalize_config(&copy) == kNormalizeNone;
}

bool config_has_valid_sta_settings()
{
    EspConfig copy = g_config;

    (void)normalize_config(&copy);
    if (copy.wifi_ssid[0] == '\0') return false;
    if (copy.wifi_password[0] != '\0') {
        size_t pass_len = strlen(copy.wifi_password);
        if (pass_len < 8u || pass_len > 63u) return false;
    }
    if (!copy.use_dhcp) {
        if (!subnet_is_valid(copy.mask)) return false;
        if (!host_is_valid_for_mask(copy.ip, copy.mask)) return false;
        if (!host_is_valid_for_mask(copy.gw, copy.mask)) return false;
        if (!same_subnet(copy.ip, copy.gw, copy.mask)) return false;
        if (!nonzero_ip_is_valid(copy.dns)) return false;
    }
    return true;
}

void config_init()
{
    (void)loadConfig();
}

bool config_load()
{
    return loadConfig();
}

void config_save()
{
    (void)saveConfig();
}

void config_reset_defaults()
{
    resetConfigToDefaults();
}

void config_mark_running_from_ram_recovery(const char *reason, bool save_required)
{
    mark_running_from_ram_recovery(reason, save_required);
}
