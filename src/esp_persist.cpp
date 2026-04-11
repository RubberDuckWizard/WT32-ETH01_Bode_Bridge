#include <EEPROM.h>
#include <Preferences.h>
#include <ctype.h>
#include <string.h>

#include "esp_persist.h"
#include "esp_config.h"
#include "esp_fy6900.h"

EspConfig g_config;
static bool s_store_was_valid = false;

namespace {

Preferences s_prefs;

const char *const kPrefsNamespace = "espbode";
const uint8_t kIpv4Len = 4u;

static const uint8_t kDefIp[] = { DEF_IP };
static const uint8_t kDefMask[] = { DEF_MASK };
static const uint8_t kDefGw[] = { DEF_GW };
static const uint8_t kDefDns[] = { DEF_DNS };
static const uint8_t kDefDns2[] = { DEF_DNS2 };
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

static void set_default_config(EspConfig *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->version = CONFIG_VERSION;
    cfg->use_dhcp = DEF_USE_DHCP;
    cfg->recovery_ap_enable = DEF_RECOVERY_AP_ENABLE;
    cfg->scope_http_proxy_enable = DEF_SCOPE_HTTP_PROXY_ENABLE;

    copy_ip(cfg->ip, kDefIp);
    copy_ip(cfg->mask, kDefMask);
    copy_ip(cfg->gw, kDefGw);
    copy_ip(cfg->dns, kDefDns);
    copy_ip(cfg->dns2, kDefDns2);
    copy_ip(cfg->lan_ip, kDefLanIp);
    copy_ip(cfg->lan_mask, kDefLanMask);
    copy_ip(cfg->scope_ip, kDefScopeIp);

    copy_text(cfg->device_hostname, sizeof(cfg->device_hostname), DEF_DEVICE_HOSTNAME);
    copy_text(cfg->wifi_ssid, sizeof(cfg->wifi_ssid), DEF_WIFI_SSID);
    copy_text(cfg->wifi_password, sizeof(cfg->wifi_password), DEF_WIFI_PASSWORD);
    copy_text(cfg->ap_ssid, sizeof(cfg->ap_ssid), DEF_RECOVERY_AP_SSID);
    copy_text(cfg->ap_password, sizeof(cfg->ap_password), DEF_RECOVERY_AP_PASSWORD);
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

static bool normalize_config(EspConfig *cfg)
{
    bool valid = true;

    cfg->version = CONFIG_VERSION;
    cfg->device_hostname[sizeof(cfg->device_hostname) - 1] = '\0';
    cfg->wifi_ssid[sizeof(cfg->wifi_ssid) - 1] = '\0';
    cfg->wifi_password[sizeof(cfg->wifi_password) - 1] = '\0';
    cfg->ap_ssid[sizeof(cfg->ap_ssid) - 1] = '\0';
    cfg->ap_password[sizeof(cfg->ap_password) - 1] = '\0';
    cfg->friendly_name[sizeof(cfg->friendly_name) - 1] = '\0';
    cfg->idn_response_name[sizeof(cfg->idn_response_name) - 1] = '\0';
    cfg->awg_serial_mode[sizeof(cfg->awg_serial_mode) - 1] = '\0';

    if (!hostname_is_valid(cfg->device_hostname)) {
        copy_text(cfg->device_hostname, sizeof(cfg->device_hostname), DEF_DEVICE_HOSTNAME);
        valid = false;
    }
    if (!recovery_ap_ssid_is_valid(cfg->ap_ssid)) {
        copy_text(cfg->ap_ssid, sizeof(cfg->ap_ssid), DEF_RECOVERY_AP_SSID);
        valid = false;
    }
    if (!recovery_ap_password_is_valid(cfg->ap_password)) {
        copy_text(cfg->ap_password, sizeof(cfg->ap_password), DEF_RECOVERY_AP_PASSWORD);
        valid = false;
    }
    if (!safe_ascii_text(cfg->friendly_name, sizeof(cfg->friendly_name) - 1, true)) {
        copy_text(cfg->friendly_name, sizeof(cfg->friendly_name), DEF_FRIENDLY_NAME);
        valid = false;
    }
    if (!idn_name_is_valid(cfg->idn_response_name)) {
        copy_text(cfg->idn_response_name, sizeof(cfg->idn_response_name), DEF_IDN_RESPONSE_NAME);
        valid = false;
    }
    if (!fy_is_supported_serial_mode(cfg->awg_serial_mode)) {
        copy_text(cfg->awg_serial_mode, sizeof(cfg->awg_serial_mode), DEF_AWG_SERIAL_MODE);
        valid = false;
    }
    if (!fy_is_supported_baud(cfg->awg_baud)) {
        cfg->awg_baud = AWG_BAUD_RATE;
        valid = false;
    }
    if (!fy_is_supported_firmware_family(cfg->awg_firmware_family)) {
        cfg->awg_firmware_family = DEF_AWG_FW_FAMILY;
        valid = false;
    }
    if (cfg->scope_port == 0u) {
        cfg->scope_port = DEF_SCOPE_PORT;
        valid = false;
    }
    if (cfg->scope_connect_timeout_ms < MIN_SCOPE_TIMEOUT_MS || cfg->scope_connect_timeout_ms > MAX_SCOPE_TIMEOUT_MS) {
        cfg->scope_connect_timeout_ms = DEF_SCOPE_CONNECT_TIMEOUT_MS;
        valid = false;
    }
    if (cfg->scope_probe_interval_ms < MIN_SCOPE_INTERVAL_MS || cfg->scope_probe_interval_ms > MAX_SCOPE_INTERVAL_MS) {
        cfg->scope_probe_interval_ms = DEF_SCOPE_PROBE_INTERVAL_MS;
        valid = false;
    }
    if (cfg->vxi_session_timeout_ms < MIN_VXI_TIMEOUT_MS || cfg->vxi_session_timeout_ms > MAX_VXI_TIMEOUT_MS) {
        cfg->vxi_session_timeout_ms = DEF_VXI_SESSION_TIMEOUT_MS;
        valid = false;
    }
    if (cfg->awg_serial_timeout_ms < MIN_AWG_TIMEOUT_MS || cfg->awg_serial_timeout_ms > MAX_AWG_TIMEOUT_MS) {
        cfg->awg_serial_timeout_ms = DEF_AWG_SERIAL_TIMEOUT_MS;
        valid = false;
    }
    if (cfg->auto_output_off_timeout_ms < MIN_AUTO_OFF_TIMEOUT_MS || cfg->auto_output_off_timeout_ms > MAX_AUTO_OFF_TIMEOUT_MS) {
        cfg->auto_output_off_timeout_ms = DEF_AUTO_OUTPUT_OFF_TIMEOUT_MS;
        valid = false;
    }
    if (cfg->recovery_ap_enable > 1u) {
        cfg->recovery_ap_enable = DEF_RECOVERY_AP_ENABLE;
        valid = false;
    }
    if (cfg->scope_http_proxy_enable > 1u) {
        cfg->scope_http_proxy_enable = DEF_SCOPE_HTTP_PROXY_ENABLE;
        valid = false;
    }

    if (!subnet_is_valid(cfg->mask)
            || !host_is_valid_for_mask(cfg->ip, cfg->mask)
            || !host_is_valid_for_mask(cfg->gw, cfg->mask)
            || !same_subnet(cfg->ip, cfg->gw, cfg->mask)) {
        copy_ip(cfg->ip, kDefIp);
        copy_ip(cfg->mask, kDefMask);
        copy_ip(cfg->gw, kDefGw);
        copy_ip(cfg->dns, kDefDns);
        copy_ip(cfg->dns2, kDefDns2);
        valid = false;
    }
    if (!nonzero_ip_is_valid(cfg->dns)) {
        copy_ip(cfg->dns, kDefDns);
        valid = false;
    }
    if (!ip_is_zero(cfg->dns2) && !nonzero_ip_is_valid(cfg->dns2)) {
        copy_ip(cfg->dns2, kDefDns2);
        valid = false;
    }
#if ENABLE_ETH_RUNTIME
    if (!subnet_is_valid(cfg->lan_mask) || !host_is_valid_for_mask(cfg->lan_ip, cfg->lan_mask)) {
        copy_ip(cfg->lan_ip, kDefLanIp);
        copy_ip(cfg->lan_mask, kDefLanMask);
        valid = false;
    }
    if (!nonzero_ip_is_valid(cfg->scope_ip)
            || !same_subnet(cfg->scope_ip, cfg->lan_ip, cfg->lan_mask)
            || ip_to_u32(cfg->scope_ip) == ip_to_u32(cfg->lan_ip)) {
        normalize_scope_ip_for_lan(cfg->scope_ip, cfg->lan_ip, cfg->lan_mask);
        valid = false;
    }
    if (!cfg->use_dhcp && same_subnet(cfg->ip, cfg->lan_ip, cfg->lan_mask)) {
        cfg->use_dhcp = 1u;
        valid = false;
    }
#else
    if (!nonzero_ip_is_valid(cfg->scope_ip)) {
        copy_ip(cfg->scope_ip, kDefScopeIp);
        valid = false;
    }
#endif

    if (cfg->wifi_ssid[0] != '\0' && !safe_ascii_text(cfg->wifi_ssid, sizeof(cfg->wifi_ssid) - 1, true)) {
        cfg->wifi_ssid[0] = '\0';
        cfg->wifi_password[0] = '\0';
        valid = false;
    }
    if (cfg->wifi_ssid[0] == '\0' && cfg->wifi_password[0] != '\0') {
        cfg->wifi_password[0] = '\0';
        valid = false;
    }
    if (cfg->wifi_password[0] != '\0') {
        size_t pass_len = strlen(cfg->wifi_password);
        if (pass_len < 8u || pass_len > 63u || !safe_ascii_text(cfg->wifi_password, sizeof(cfg->wifi_password) - 1, true)) {
            cfg->wifi_password[0] = '\0';
            valid = false;
        }
    }

    return valid;
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

static bool migrate_legacy_eeprom()
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
    (void)saveConfig();
    return true;
}

static void load_from_preferences()
{
    set_default_config(&g_config);

    g_config.version = s_prefs.getUChar("version", CONFIG_VERSION);
    g_config.use_dhcp = s_prefs.getBool("dhcp", DEF_USE_DHCP) ? 1u : 0u;
    g_config.recovery_ap_enable = s_prefs.getBool("recovery_ap", DEF_RECOVERY_AP_ENABLE) ? 1u : 0u;
    g_config.scope_http_proxy_enable = s_prefs.getBool("scope_proxy", DEF_SCOPE_HTTP_PROXY_ENABLE) ? 1u : 0u;
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
    (void)s_prefs.getString("friendly", g_config.friendly_name, sizeof(g_config.friendly_name));
    (void)s_prefs.getString("idn_name", g_config.idn_response_name, sizeof(g_config.idn_response_name));
    (void)s_prefs.getString("fy_mode", g_config.awg_serial_mode, sizeof(g_config.awg_serial_mode));
}

}  // namespace

bool loadConfig()
{
    set_default_config(&g_config);
    s_store_was_valid = false;

    if (!s_prefs.begin(kPrefsNamespace, false)) {
        return false;
    }

    if (!s_prefs.isKey("version")) {
        s_prefs.end();
        if (migrate_legacy_eeprom()) {
            s_store_was_valid = true;
            return true;
        }
        set_default_config(&g_config);
        return false;
    }

    load_from_preferences();
    s_prefs.end();

    s_store_was_valid = normalize_config(&g_config);
    if (!s_store_was_valid) {
        (void)saveConfig();
    }
    return s_store_was_valid;
}

bool saveConfig()
{
    (void)normalize_config(&g_config);

    if (!s_prefs.begin(kPrefsNamespace, false)) {
        return false;
    }

    s_prefs.putUChar("version", CONFIG_VERSION);
    s_prefs.putBool("dhcp", g_config.use_dhcp != 0u);
    s_prefs.putBool("recovery_ap", g_config.recovery_ap_enable != 0u);
    s_prefs.putBool("scope_proxy", g_config.scope_http_proxy_enable != 0u);
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
    s_prefs.putString("friendly", g_config.friendly_name);
    s_prefs.putString("idn_name", g_config.idn_response_name);
    s_prefs.putString("fy_mode", g_config.awg_serial_mode);
    s_prefs.end();

    s_store_was_valid = config_current_is_valid();
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

bool config_current_is_valid()
{
    EspConfig copy = g_config;
    return normalize_config(&copy);
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
    if (!loadConfig()) {
        resetConfigToDefaults();
        (void)saveConfig();
    }
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
