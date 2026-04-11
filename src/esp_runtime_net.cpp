#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <time.h>

#include "esp_runtime_net.h"
#include "esp_config.h"
#include "esp_diag_console.h"
#include "esp_fy6900.h"
#include "esp_network.h"
#include "esp_persist.h"

#if ENABLE_ETH_RUNTIME
#include "esp_eth.h"
#endif

namespace {

struct RuntimeNetState {
    bool web_ready;
    bool bode_ready;
    bool scope_probe_available;
    bool scope_reachable;
    uint32_t scope_last_check_ms;
    uint32_t scope_last_success_ms;
    bool sta_valid;
    bool sta_connecting;
    bool sta_connected;
    bool sta_retry_exhausted;
    bool ap_active;
    bool lan_started;
    bool lan_link_up;
    bool lan_has_ip;
    bool time_sync_started;
    bool time_valid;
    uint32_t sta_connect_deadline_ms;
    uint32_t sta_retry_after_ms;
    uint32_t last_time_sync_attempt_ms;
    uint8_t sta_retry_count;
    String mac;
    String sta_ssid;
    String ap_ssid;
    char last_fail_reason[40];
};

RuntimeNetState s_state;
uint32_t s_last_runtime_heartbeat_ms = 0;
bool s_runtime_snapshot_known = false;
bool s_prev_web_ready = false;
bool s_prev_bode_ready = false;
bool s_prev_sta_connecting = false;
bool s_prev_sta_connected = false;
bool s_prev_ap_active = false;
bool s_prev_lan_started = false;
bool s_prev_lan_link_up = false;
bool s_prev_lan_has_ip = false;
char s_prev_fail_reason[40] = "";

#if ENABLE_MINIMAL_LAN_NTP
WiFiUDP s_ntp_udp;
bool s_ntp_server_started = false;
uint32_t s_ntp_request_count = 0;
uint32_t s_ntp_last_served_ms = 0;
#endif

static void set_fail_reason(const char *reason)
{
    if (reason == NULL) reason = "none";
    strncpy(s_state.last_fail_reason, reason, sizeof(s_state.last_fail_reason) - 1);
    s_state.last_fail_reason[sizeof(s_state.last_fail_reason) - 1] = '\0';
}

static void clear_fail_reason(void)
{
    set_fail_reason("none");
}

static IPAddress ip_from_bytes(const uint8_t *ip4)
{
    return IPAddress(ip4[0], ip4[1], ip4[2], ip4[3]);
}

static bool ip_is_zero(IPAddress ip)
{
    return ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] == 0;
}

static uint32_t ip_to_u32(IPAddress ip)
{
    return ((uint32_t)ip[0] << 24)
        | ((uint32_t)ip[1] << 16)
        | ((uint32_t)ip[2] << 8)
        | (uint32_t)ip[3];
}

static bool same_subnet(IPAddress left, IPAddress right, IPAddress mask)
{
    uint32_t mask_u32 = ip_to_u32(mask);
    return (ip_to_u32(left) & mask_u32) == (ip_to_u32(right) & mask_u32);
}

static const char *bode_runtime_text(void)
{
    if (net_is_running()) return "running";
    if (s_state.bode_ready) return "ready";
    return "off";
}

static void print_runtime_snapshot(const char *reason)
{
    Serial.printf("[diag] %s lan=%s/%s/%s ip=%s sta=%s/%s ip=%s ap=%s ip=%s web=%s bode=%s ntp=%s/%lu scope=%s/%s fy=%s fail=%s\r\n",
        reason,
        s_state.lan_started ? "start" : "idle",
        s_state.lan_link_up ? "link" : "nolink",
        s_state.lan_has_ip ? "ip" : "noip",
        runtime_net_lan_ip().toString().c_str(),
        s_state.sta_valid ? "cfg" : "nocfg",
        s_state.sta_connected ? "up" : (s_state.sta_connecting ? "try" : "down"),
        runtime_net_sta_ip().toString().c_str(),
        s_state.ap_active ? "on" : "off",
        runtime_net_ap_ip().toString().c_str(),
        s_state.web_ready ? "on" : "off",
        bode_runtime_text(),
        runtime_net_ntp_server_running() ? "on" : "off",
        (unsigned long)runtime_net_ntp_request_count(),
        s_state.scope_probe_available ? "on" : "off",
        s_state.scope_reachable ? "ok" : "fail",
        fy_status_text(),
        s_state.last_fail_reason);
}

static void poll_runtime_heartbeat(void)
{
    bool changed = !s_runtime_snapshot_known
        || s_prev_web_ready != s_state.web_ready
        || s_prev_bode_ready != s_state.bode_ready
        || s_prev_sta_connecting != s_state.sta_connecting
        || s_prev_sta_connected != s_state.sta_connected
        || s_prev_ap_active != s_state.ap_active
        || s_prev_lan_started != s_state.lan_started
        || s_prev_lan_link_up != s_state.lan_link_up
        || s_prev_lan_has_ip != s_state.lan_has_ip
        || strcmp(s_prev_fail_reason, s_state.last_fail_reason) != 0;
    bool heartbeat_due = ENABLE_RUNTIME_HEARTBEAT
        && (!s_runtime_snapshot_known
            || (uint32_t)(millis() - s_last_runtime_heartbeat_ms) >= 5000U);

    if (!changed && !heartbeat_due) return;

    print_runtime_snapshot(changed ? "state" : "beat");
    s_last_runtime_heartbeat_ms = millis();
    s_runtime_snapshot_known = true;
    s_prev_web_ready = s_state.web_ready;
    s_prev_bode_ready = s_state.bode_ready;
    s_prev_sta_connecting = s_state.sta_connecting;
    s_prev_sta_connected = s_state.sta_connected;
    s_prev_ap_active = s_state.ap_active;
    s_prev_lan_started = s_state.lan_started;
    s_prev_lan_link_up = s_state.lan_link_up;
    s_prev_lan_has_ip = s_state.lan_has_ip;
    strncpy(s_prev_fail_reason, s_state.last_fail_reason, sizeof(s_prev_fail_reason) - 1);
    s_prev_fail_reason[sizeof(s_prev_fail_reason) - 1] = '\0';
}

static void update_runtime_readiness(void)
{
    s_state.web_ready = s_state.ap_active || s_state.sta_connected || s_state.lan_has_ip;
    s_state.bode_ready = s_state.lan_has_ip;

    if (s_state.sta_connected) {
        s_state.mac = WiFi.macAddress();
    } else if (s_state.ap_active) {
        s_state.mac = WiFi.softAPmacAddress();
#if ENABLE_ETH_RUNTIME
    } else if (s_state.lan_started) {
        s_state.mac = eth_mac_string();
#endif
    } else {
        s_state.mac = "";
    }
}

static void refresh_lan_state(void)
{
#if ENABLE_ETH_RUNTIME
    s_state.lan_started = eth_started();
    s_state.lan_link_up = eth_link_is_up();
    s_state.lan_has_ip = eth_is_connected();
#else
    s_state.lan_started = false;
    s_state.lan_link_up = false;
    s_state.lan_has_ip = false;
#endif
}

static bool recovery_ap_allowed(void)
{
    return FW_KEEP_RECOVERY_AP_ALWAYS_ON
        || FW_FORCE_RECOVERY_AP
        || g_config.recovery_ap_enable != 0u
        || FW_SAFE_RECOVERY_FALLBACK
        || FW_START_AP_DURING_STA_BOOT;
}

static bool recovery_ap_should_stay_active(void)
{
    return FW_KEEP_RECOVERY_AP_ALWAYS_ON || FW_FORCE_RECOVERY_AP;
}

static bool recovery_ap_uses_open_security(void)
{
    return FW_FORCE_RECOVERY_AP != 0 || g_config.ap_password[0] == '\0';
}

static const char *configured_ap_ssid(void)
{
    return g_config.ap_ssid[0] ? g_config.ap_ssid : RECOVERY_AP_SSID;
}

static const char *configured_ap_password(void)
{
    return recovery_ap_uses_open_security() ? NULL : g_config.ap_password;
}

static bool sta_retries_are_finite(void)
{
    return STA_CONNECT_MAX_ATTEMPTS != 0u;
}

static void log_sta_attempt_window(const char *reason, uint32_t delay_ms)
{
    Serial.printf("[wifi] %s delay_ms=%lu retry_count=%u retry_limit=%u ssid=%s\r\n",
        reason,
        (unsigned long)delay_ms,
        (unsigned)s_state.sta_retry_count,
        (unsigned)STA_CONNECT_MAX_ATTEMPTS,
        g_config.wifi_ssid);
}

static void reset_wifi_radio(void)
{
    WiFi.persistent(false);
    WiFi.disconnect(false, false);
    WiFi.mode(WIFI_MODE_NULL);
    delay(20);
}

static bool start_recovery_ap(void)
{
    static const uint8_t ap_ip_bytes[] = { RECOVERY_AP_IP };
    static const uint8_t ap_mask_bytes[] = { RECOVERY_AP_MASK };
    IPAddress ap_ip = ip_from_bytes(ap_ip_bytes);
    IPAddress ap_mask = ip_from_bytes(ap_mask_bytes);
    wifi_mode_t mode = s_state.sta_valid ? WIFI_AP_STA : WIFI_AP;
    const char *ap_ssid = configured_ap_ssid();
    const char *ap_password = configured_ap_password();

    if (s_state.ap_active) {
        return true;
    }
    if (!recovery_ap_allowed()) {
        set_fail_reason("recovery_ap_disabled");
        return false;
    }

    WiFi.mode(mode);
    if (!WiFi.softAPConfig(ap_ip, ap_ip, ap_mask)) {
        set_fail_reason("ap_config_failed");
        return false;
    }
    if (!WiFi.softAP(ap_ssid, ap_password, RECOVERY_AP_CHANNEL, 0, RECOVERY_AP_MAX_CLIENTS)) {
        set_fail_reason("ap_start_failed");
        return false;
    }

    s_state.ap_active = true;
    s_state.ap_ssid = ap_ssid;
    Serial.printf("[wifi] recovery ap ip=%s ssid=%s security=%s\r\n",
        WiFi.softAPIP().toString().c_str(),
        ap_ssid,
        ap_password != NULL ? "secured" : "open");
    update_runtime_readiness();
    return true;
}

static void stop_recovery_ap(void)
{
    if (!s_state.ap_active) return;

    WiFi.softAPdisconnect(true);
    s_state.ap_active = false;
    s_state.ap_ssid = "";

    if (s_state.sta_connected || s_state.sta_connecting) {
        WiFi.mode(WIFI_STA);
    }
    update_runtime_readiness();
}

static bool start_sta_connect(void)
{
    const char *password = g_config.wifi_password[0] ? g_config.wifi_password : NULL;

    if (!s_state.sta_valid || g_config.wifi_ssid[0] == '\0') {
        return false;
    }
    if (sta_retries_are_finite() && s_state.sta_retry_count >= STA_CONNECT_MAX_ATTEMPTS) {
        s_state.sta_retry_exhausted = true;
        return false;
    }

    WiFi.setHostname(g_config.device_hostname);
    WiFi.setAutoReconnect(true);
    WiFi.mode(s_state.ap_active ? WIFI_AP_STA : WIFI_STA);

    if (!g_config.use_dhcp) {
        if (!WiFi.config(ip_from_bytes(g_config.ip), ip_from_bytes(g_config.gw),
                ip_from_bytes(g_config.mask), ip_from_bytes(g_config.dns),
                ip_from_bytes(g_config.dns2))) {
            set_fail_reason("sta_config_failed");
            return false;
        }
    } else {
        IPAddress zero((uint32_t)0);
        (void)WiFi.config(zero, zero, zero, zero, zero);
    }

    WiFi.begin(g_config.wifi_ssid, password);
    s_state.sta_retry_count++;
    s_state.sta_connecting = true;
    s_state.sta_connected = false;
    s_state.sta_retry_exhausted = false;
    s_state.sta_connect_deadline_ms = millis() + STA_CONNECT_TIMEOUT_MS;
    s_state.sta_ssid = g_config.wifi_ssid;
    set_fail_reason("sta_connecting");
    Serial.printf("[wifi] sta begin attempt=%u/%u ssid=%s dhcp=%s\r\n",
        (unsigned)s_state.sta_retry_count,
        (unsigned)STA_CONNECT_MAX_ATTEMPTS,
        g_config.wifi_ssid,
        g_config.use_dhcp ? "on" : "off");
    update_runtime_readiness();
    return true;
}

static void ensure_recovery_ap(const char *reason)
{
    if (reason != NULL && reason[0] != '\0') {
        set_fail_reason(reason);
    }
    if (start_recovery_ap()) {
        Serial.printf("[wifi] recovery AP active reason=%s ip=%s\r\n",
            s_state.last_fail_reason,
            WiFi.softAPIP().toString().c_str());
    }
}

static void poll_sta_connection(void)
{
    if (!s_state.sta_valid) return;

    if (s_state.sta_connecting) {
        if (WiFi.status() == WL_CONNECTED && !ip_is_zero(WiFi.localIP())) {
            s_state.sta_connecting = false;
            s_state.sta_connected = true;
            s_state.sta_retry_after_ms = 0;
            s_state.sta_retry_exhausted = false;
            s_state.sta_ssid = WiFi.SSID();
            clear_fail_reason();
            if (s_state.ap_active && !recovery_ap_should_stay_active()) {
                stop_recovery_ap();
            }
            Serial.printf("[wifi] sta connected attempt=%u/%u ip=%s ssid=%s rssi=%ld ap=%s\r\n",
                (unsigned)s_state.sta_retry_count,
                (unsigned)STA_CONNECT_MAX_ATTEMPTS,
                WiFi.localIP().toString().c_str(),
                WiFi.SSID().c_str(),
                (long)WiFi.RSSI(),
                s_state.ap_active ? "on" : "off");
            update_runtime_readiness();
            return;
        }

        if ((int32_t)(millis() - s_state.sta_connect_deadline_ms) >= 0) {
            s_state.sta_connecting = false;
            s_state.sta_connected = false;
            WiFi.disconnect(false, false);
            if (sta_retries_are_finite() && s_state.sta_retry_count >= STA_CONNECT_MAX_ATTEMPTS) {
                s_state.sta_retry_after_ms = 0;
                s_state.sta_retry_exhausted = true;
                ensure_recovery_ap("sta_retry_exhausted");
                Serial.printf("[wifi] sta retries exhausted attempts=%u/%u keeping recovery AP active\r\n",
                    (unsigned)s_state.sta_retry_count,
                    (unsigned)STA_CONNECT_MAX_ATTEMPTS);
            } else {
                s_state.sta_retry_after_ms = millis() + STA_RETRY_INTERVAL_MS;
                ensure_recovery_ap("sta_connect_failed");
                Serial.printf("[wifi] sta attempt=%u/%u failed next_retry_in_ms=%u\r\n",
                    (unsigned)s_state.sta_retry_count,
                    (unsigned)STA_CONNECT_MAX_ATTEMPTS,
                    (unsigned)STA_RETRY_INTERVAL_MS);
            }
            update_runtime_readiness();
        }
        return;
    }

    if (s_state.sta_connected) {
        bool connected = WiFi.status() == WL_CONNECTED && !ip_is_zero(WiFi.localIP());
        if (connected) {
            s_state.sta_ssid = WiFi.SSID();
            update_runtime_readiness();
            return;
        }

        s_state.sta_connected = false;
        s_state.sta_retry_count = 0u;
        s_state.sta_retry_exhausted = false;
        s_state.sta_retry_after_ms = millis() + STA_RETRY_INTERVAL_MS;
        ensure_recovery_ap("sta_lost_connection");
        Serial.printf("[wifi] sta lost connection ssid=%s retry_cycle_restart_in_ms=%u\r\n",
            s_state.sta_ssid.c_str(),
            (unsigned)STA_RETRY_INTERVAL_MS);
        update_runtime_readiness();
        return;
    }

    if (s_state.sta_retry_exhausted) {
        return;
    }

    if (s_state.sta_retry_after_ms != 0
            && (int32_t)(millis() - s_state.sta_retry_after_ms) >= 0) {
        s_state.sta_retry_after_ms = 0;
        if (!start_sta_connect() && s_state.sta_retry_exhausted) {
            ensure_recovery_ap("sta_retry_exhausted");
        }
    }
}

#if ENABLE_MINIMAL_LAN_NTP
static bool ntp_time_valid(void)
{
    time_t now = time(NULL);
    return now >= (time_t)NTP_VALID_UNIX_THRESHOLD;
}

static void put_u32_be(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value >> 24);
    dst[1] = (uint8_t)(value >> 16);
    dst[2] = (uint8_t)(value >> 8);
    dst[3] = (uint8_t)value;
}

static void put_u64_be(uint8_t *dst, uint64_t value)
{
    put_u32_be(dst, (uint32_t)(value >> 32));
    put_u32_be(dst + 4, (uint32_t)value);
}

static uint64_t unix_time_to_ntp64(void)
{
    struct timeval tv;
    uint64_t seconds;
    uint64_t fraction;

    gettimeofday(&tv, NULL);
    seconds = (uint64_t)tv.tv_sec + 2208988800ULL;
    fraction = ((uint64_t)tv.tv_usec << 32) / 1000000ULL;
    return (seconds << 32) | fraction;
}

static bool ntp_remote_on_lan(IPAddress remote)
{
    if (!s_state.lan_has_ip) return false;
    return same_subnet(remote, runtime_net_lan_ip(), runtime_net_lan_subnet());
}

static void poll_time_sync(void)
{
    bool valid_now = ntp_time_valid();

    if (valid_now) {
        s_state.time_valid = true;
        return;
    }

    s_state.time_valid = false;
    if (!s_state.sta_connected) return;
    if (s_state.time_sync_started
            && (uint32_t)(millis() - s_state.last_time_sync_attempt_ms) < NTP_SYNC_RETRY_INTERVAL_MS) {
        return;
    }

    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    s_state.time_sync_started = true;
    s_state.last_time_sync_attempt_ms = millis();
}

static void start_ntp_server_if_needed(void)
{
    if (s_ntp_server_started) return;
    s_ntp_server_started = s_ntp_udp.begin(LAN_NTP_PORT);
    Serial.printf("[ntp] server=%s port=%u lan_ip=%s mask=%s\r\n",
        s_ntp_server_started ? "on" : "off",
        (unsigned)LAN_NTP_PORT,
        runtime_net_lan_ip().toString().c_str(),
        runtime_net_lan_subnet().toString().c_str());
}

static void poll_ntp_server(void)
{
    uint8_t request[48];
    static uint32_t s_last_ntp_drop_log_ms = 0;

    if (!s_ntp_server_started) return;

    for (;;) {
        int packet_len = s_ntp_udp.parsePacket();
        if (packet_len <= 0) break;

        memset(request, 0, sizeof(request));
        if (s_ntp_udp.read(request, sizeof(request)) < 48) {
            if ((uint32_t)(millis() - s_last_ntp_drop_log_ms) >= 3000U) {
                s_last_ntp_drop_log_ms = millis();
                Serial.printf("[ntp] drop reason=short-packet remote=%s:%u\r\n",
                    s_ntp_udp.remoteIP().toString().c_str(),
                    (unsigned)s_ntp_udp.remotePort());
            }
            continue;
        }
        if (!ntp_remote_on_lan(s_ntp_udp.remoteIP())) {
            if ((uint32_t)(millis() - s_last_ntp_drop_log_ms) >= 3000U) {
                s_last_ntp_drop_log_ms = millis();
                Serial.printf("[ntp] drop reason=not-on-lan remote=%s:%u lan=%s/%s has_ip=%s\r\n",
                    s_ntp_udp.remoteIP().toString().c_str(),
                    (unsigned)s_ntp_udp.remotePort(),
                    runtime_net_lan_ip().toString().c_str(),
                    runtime_net_lan_subnet().toString().c_str(),
                    s_state.lan_has_ip ? "yes" : "no");
            }
            continue;
        }

        uint8_t response[48];
        uint8_t version = (uint8_t)((request[0] >> 3) & 0x7u);
        bool time_valid = ntp_time_valid();
        uint64_t now_ntp = time_valid ? unix_time_to_ntp64() : 0ULL;

        if (version < 3u || version > 4u) version = 4u;

        memset(response, 0, sizeof(response));
        response[0] = (uint8_t)(((time_valid ? 0u : 3u) << 6) | (version << 3) | 4u);
        response[1] = time_valid ? 2u : 16u;
        response[2] = request[2];
        response[3] = (uint8_t)-20;
        response[12] = time_valid ? 'W' : 'I';
        response[13] = time_valid ? 'I' : 'N';
        response[14] = time_valid ? 'F' : 'I';
        response[15] = time_valid ? 'I' : 'T';
        memcpy(response + 24, request + 40, 8u);
        put_u64_be(response + 16, now_ntp);
        put_u64_be(response + 32, now_ntp);
        put_u64_be(response + 40, now_ntp);

        s_ntp_udp.beginPacket(s_ntp_udp.remoteIP(), s_ntp_udp.remotePort());
        s_ntp_udp.write(response, sizeof(response));
        s_ntp_udp.endPacket();

        s_ntp_request_count++;
        s_ntp_last_served_ms = millis();
        s_state.time_valid = time_valid;
        Serial.printf("[ntp] served remote=%s:%u lan=%s/%s time=%s requests=%lu\r\n",
            s_ntp_udp.remoteIP().toString().c_str(),
            (unsigned)s_ntp_udp.remotePort(),
            runtime_net_lan_ip().toString().c_str(),
            runtime_net_lan_subnet().toString().c_str(),
            runtime_net_time_status_text(),
            (unsigned long)s_ntp_request_count);
    }
}
#else
static void poll_time_sync(void)
{
}

static void start_ntp_server_if_needed(void)
{
}

static void poll_ntp_server(void)
{
}
#endif

static void probe_scope_if_needed(void)
{
    WiFiClient client;
    uint32_t now = millis();
    static bool s_scope_log_known = false;
    static bool s_scope_log_available = false;
    static bool s_scope_log_reachable = false;

    if (!s_state.lan_has_ip) {
        s_state.scope_probe_available = false;
        s_state.scope_reachable = false;
        return;
    }
    if (g_config.scope_ip[0] == 0 && g_config.scope_ip[1] == 0
            && g_config.scope_ip[2] == 0 && g_config.scope_ip[3] == 0) {
        s_state.scope_probe_available = false;
        s_state.scope_reachable = false;
        return;
    }
    if (s_state.scope_last_check_ms != 0
            && (uint32_t)(now - s_state.scope_last_check_ms) < g_config.scope_probe_interval_ms) {
        return;
    }

    s_state.scope_probe_available = true;
    s_state.scope_last_check_ms = now;
    s_state.scope_reachable = false;

    if (client.connect(ip_from_bytes(g_config.scope_ip), g_config.scope_port,
            g_config.scope_connect_timeout_ms)) {
        s_state.scope_reachable = true;
        s_state.scope_last_success_ms = now;
        client.stop();
    }

    if (!s_scope_log_known
            || s_scope_log_available != s_state.scope_probe_available
            || s_scope_log_reachable != s_state.scope_reachable) {
        s_scope_log_known = true;
        s_scope_log_available = s_state.scope_probe_available;
        s_scope_log_reachable = s_state.scope_reachable;
        Serial.printf("[scope] probe=%s reachable=%s scope=%u.%u.%u.%u:%u lan=%s/%s\r\n",
            s_state.scope_probe_available ? "on" : "off",
            s_state.scope_reachable ? "yes" : "no",
            g_config.scope_ip[0], g_config.scope_ip[1], g_config.scope_ip[2], g_config.scope_ip[3],
            (unsigned)g_config.scope_port,
            runtime_net_lan_ip().toString().c_str(),
            runtime_net_lan_subnet().toString().c_str());
    }
}

}  // namespace

bool runtime_net_begin(void)
{
    s_state = RuntimeNetState();
    clear_fail_reason();
    s_last_runtime_heartbeat_ms = 0;
    s_runtime_snapshot_known = false;
    s_prev_fail_reason[0] = '\0';

    reset_wifi_radio();

#if ENABLE_ETH_RUNTIME
    if (!eth_begin_runtime()) {
        set_fail_reason(eth_last_fail_reason_text());
    }
#endif
    s_state.sta_valid = config_has_valid_sta_settings();
    refresh_lan_state();

    if (FW_FORCE_RECOVERY_AP) {
        ensure_recovery_ap("forced_recovery_ap");
    } else if (!s_state.sta_valid) {
        ensure_recovery_ap("no_sta_config");
    } else if (FW_KEEP_RECOVERY_AP_ALWAYS_ON) {
        ensure_recovery_ap("permanent_ap");
    } else if (FW_START_AP_DURING_STA_BOOT) {
        ensure_recovery_ap("sta_bootstrap_ap");
    }

    if (s_state.sta_valid) {
        if (STA_BOOT_CONNECT_DELAY_MS > 0u) {
            s_state.sta_retry_after_ms = millis() + STA_BOOT_CONNECT_DELAY_MS;
            set_fail_reason("sta_boot_delay");
            log_sta_attempt_window("sta delayed start", STA_BOOT_CONNECT_DELAY_MS);
        } else {
            (void)start_sta_connect();
        }
    }

    start_ntp_server_if_needed();
    update_runtime_readiness();
    return s_state.web_ready;
}

void runtime_net_poll(void)
{
    refresh_lan_state();
    poll_sta_connection();
    poll_time_sync();
    poll_ntp_server();
    update_runtime_readiness();
    probe_scope_if_needed();
    poll_runtime_heartbeat();
}

RuntimeNetMode runtime_net_mode(void)
{
    if (s_state.sta_connected) return RUNTIME_NET_WIFI_STA;
    if (s_state.ap_active) return RUNTIME_NET_WIFI_AP;
    if (s_state.lan_has_ip) return RUNTIME_NET_ETH;
    return RUNTIME_NET_NONE;
}

const char *runtime_net_mode_text(void)
{
    if (s_state.lan_has_ip && s_state.sta_connected && s_state.ap_active) return "LAN + WiFi STA + AP";
    if (s_state.lan_has_ip && s_state.sta_connected) return "LAN + WiFi STA";
    if (s_state.lan_has_ip && s_state.ap_active) return "LAN + Recovery AP";
    if (s_state.sta_connected && s_state.ap_active) return "WiFi STA + Recovery AP";
    if (s_state.lan_has_ip) return "LAN";
    if (s_state.sta_connected) return "WiFi STA";
    if (s_state.ap_active) return "Recovery AP";
    return "Offline";
}

bool runtime_net_has_ip(void)
{
    return !ip_is_zero(runtime_net_ip());
}

bool runtime_net_is_ready_for_web(void)
{
    return s_state.web_ready;
}

bool runtime_net_should_run_bode(void)
{
    return s_state.bode_ready;
}

bool runtime_net_recovery_ap_active(void)
{
    return s_state.ap_active;
}

bool runtime_net_sta_config_valid(void)
{
    return config_has_valid_sta_settings();
}

bool runtime_net_sta_connected(void)
{
    return s_state.sta_connected;
}

bool runtime_net_sta_connecting(void)
{
    return s_state.sta_connecting;
}

bool runtime_net_sta_retry_exhausted(void)
{
    return s_state.sta_retry_exhausted;
}

bool runtime_net_lan_started(void)
{
    return s_state.lan_started;
}

bool runtime_net_lan_link_up(void)
{
    return s_state.lan_link_up;
}

bool runtime_net_lan_has_ip(void)
{
    return s_state.lan_has_ip;
}

bool runtime_net_time_synced(void)
{
#if ENABLE_MINIMAL_LAN_NTP
    return ntp_time_valid();
#else
    return false;
#endif
}

bool runtime_net_ntp_server_running(void)
{
#if ENABLE_MINIMAL_LAN_NTP
    return s_ntp_server_started;
#else
    return false;
#endif
}

IPAddress runtime_net_ip(void)
{
    if (s_state.sta_connected) return WiFi.localIP();
    if (s_state.ap_active) return WiFi.softAPIP();
    if (s_state.lan_has_ip) return runtime_net_lan_ip();
    return IPAddress((uint32_t)0);
}

IPAddress runtime_net_gateway(void)
{
    if (s_state.sta_connected) return WiFi.gatewayIP();
    if (s_state.ap_active) return WiFi.softAPIP();
#if ENABLE_ETH_RUNTIME
    if (s_state.lan_has_ip) return eth_gateway_ip();
#endif
    return IPAddress((uint32_t)0);
}

IPAddress runtime_net_subnet(void)
{
    if (s_state.sta_connected) return WiFi.subnetMask();
    if (s_state.ap_active) return WiFi.softAPSubnetMask();
    if (s_state.lan_has_ip) return runtime_net_lan_subnet();
    return IPAddress((uint32_t)0);
}

IPAddress runtime_net_dns1(void)
{
    if (s_state.sta_connected) return WiFi.dnsIP();
#if ENABLE_ETH_RUNTIME
    if (s_state.lan_has_ip) return eth_dns_ip();
#endif
    return IPAddress((uint32_t)0);
}

IPAddress runtime_net_dns2(void)
{
    if (s_state.sta_connected) return WiFi.dnsIP(1);
    return IPAddress((uint32_t)0);
}

IPAddress runtime_net_sta_ip(void)
{
    return s_state.sta_connected ? WiFi.localIP() : IPAddress((uint32_t)0);
}

IPAddress runtime_net_ap_ip(void)
{
    return s_state.ap_active ? WiFi.softAPIP() : IPAddress((uint32_t)0);
}

IPAddress runtime_net_lan_ip(void)
{
#if ENABLE_ETH_RUNTIME
    return eth_current_ip();
#else
    return IPAddress((uint32_t)0);
#endif
}

IPAddress runtime_net_lan_subnet(void)
{
#if ENABLE_ETH_RUNTIME
    return eth_subnet_mask();
#else
    return IPAddress((uint32_t)0);
#endif
}

const char *runtime_net_mac(void)
{
    return s_state.mac.c_str();
}

const char *runtime_net_ssid(void)
{
    return s_state.sta_ssid.c_str();
}

const char *runtime_net_ap_ssid(void)
{
    return s_state.ap_ssid.c_str();
}

const char *runtime_net_time_status_text(void)
{
    if (runtime_net_time_synced()) {
        return s_state.sta_connected ? "synced via WiFi STA" : "synced (cached)";
    }
    if (s_state.sta_connected || s_state.sta_connecting) {
        return "waiting for WiFi sync";
    }
    return "unsynced";
}

int32_t runtime_net_rssi(void)
{
    if (s_state.sta_connected && WiFi.status() == WL_CONNECTED) {
        return WiFi.RSSI();
    }
    return 0;
}

uint8_t runtime_net_ap_client_count(void)
{
    return runtime_net_recovery_ap_active() ? WiFi.softAPgetStationNum() : 0;
}

uint8_t runtime_net_sta_retry_count(void)
{
    return s_state.sta_retry_count;
}

uint8_t runtime_net_sta_retry_limit(void)
{
    return (uint8_t)STA_CONNECT_MAX_ATTEMPTS;
}

const char *runtime_net_last_fail_reason(void)
{
    return s_state.last_fail_reason;
}

uint32_t runtime_net_ntp_request_count(void)
{
#if ENABLE_MINIMAL_LAN_NTP
    return s_ntp_request_count;
#else
    return 0;
#endif
}

uint32_t runtime_net_ntp_last_served_ms(void)
{
#if ENABLE_MINIMAL_LAN_NTP
    return s_ntp_last_served_ms;
#else
    return 0;
#endif
}

bool runtime_net_scope_probe_available(void)
{
    return s_state.scope_probe_available;
}

bool runtime_net_scope_reachable(void)
{
    return s_state.scope_reachable;
}

uint32_t runtime_net_scope_last_check_ms(void)
{
    return s_state.scope_last_check_ms;
}

uint32_t runtime_net_scope_last_success_ms(void)
{
    return s_state.scope_last_success_ms;
}
