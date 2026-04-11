/*
 * WT32-ETH01 / ESP32 firmware bridge for Siglent SDS800X-HD Bode mode
 * using a FeelElec / FeelTech FY6900 over TTL serial.
 */

#include <Arduino.h>
#include <WiFi.h>

#include "esp_config.h"
#include "esp_eth.h"
#include "esp_persist.h"
#include "esp_fy6900.h"
#include "esp_diag_console.h"
#include "esp_parser.h"
#include "esp_network.h"
#include "esp_runtime_net.h"
#include "esp_scope_http_proxy.h"
#include "esp_scope_vnc_proxy.h"
#include "esp_webconfig.h"

#if FW_DEFER_FY_INIT
static bool s_release_fy_init_pending = false;
static bool s_release_fy_init_done = false;
static uint32_t s_release_fy_init_after_ms = 0;
static const uint32_t RELEASE_FY_INIT_DELAY_MS = 1200U;

static void release_try_start_bode(void)
{
    if (net_is_running()) return;
    if (!runtime_net_should_run_bode()) return;

    net_begin();
    Serial.printf("release: bode services started fy_status=%s lan=%s scope=%u.%u.%u.%u:%u\r\n",
        fy_status_text(),
        runtime_net_lan_ip().toString().c_str(),
        g_config.scope_ip[0], g_config.scope_ip[1], g_config.scope_ip[2], g_config.scope_ip[3],
        (unsigned)g_config.scope_port);
}

static void release_poll_fy_init(void)
{
    if (s_release_fy_init_pending) {
        if ((int32_t)(millis() - s_release_fy_init_after_ms) < 0) {
            return;
        }

        s_release_fy_init_pending = false;
        s_release_fy_init_done = true;
        Serial.printf("release: deferred fy_init start status_before=%s serial2_initialized=%s\r\n",
            fy_status_text(),
            fy_serial_initialized() ? "yes" : "no");
        fy_init();
        Serial.printf("release: deferred fy_init done status_after=%s serial2_initialized=%s\r\n",
            fy_status_text(),
            fy_serial_initialized() ? "yes" : "no");
        if (strcmp(fy_status_text(), "available") != 0) {
            Serial.printf("release: bode services held off because fy_status=%s\r\n", fy_status_text());
        }
    }

    release_try_start_bode();
}
#endif

void setup()
{
    diag_begin_early();
    Serial.printf("\r\n\r\n=== espBode WT32-ETH01 starting ===\r\n");
    Serial.printf("variant=%s build=%s\r\n", FW_VARIANT_NAME, FW_BUILD_STRING);

    if (FW_IGNORE_STORED_CONFIG) {
        resetConfigToDefaults();
        g_config.wifi_ssid[0] = '\0';
        g_config.wifi_password[0] = '\0';
        g_config.recovery_ap_enable = 1;
        Serial.printf("safe bring-up: ignoring stored config and forcing recovery AP\r\n");
    } else {
        if (!loadConfig()) {
            resetConfigToDefaults();
            (void)saveConfig();
        }
    }

    Serial.printf("stored_config_valid=%s hostname=%s dhcp=%s heap=%lu\r\n",
        config_store_was_valid() ? "yes" : "no",
        g_config.device_hostname,
        g_config.use_dhcp ? "on" : "off",
        (unsigned long)ESP.getFreeHeap());
    if (!g_config.use_dhcp) {
        Serial.printf("static_ip=%u.%u.%u.%u mask=%u.%u.%u.%u gw=%u.%u.%u.%u dns=%u.%u.%u.%u\r\n",
            g_config.ip[0], g_config.ip[1], g_config.ip[2], g_config.ip[3],
            g_config.mask[0], g_config.mask[1], g_config.mask[2], g_config.mask[3],
            g_config.gw[0], g_config.gw[1], g_config.gw[2], g_config.gw[3],
            g_config.dns[0], g_config.dns[1], g_config.dns[2], g_config.dns[3]);
    }
#if ENABLE_ETH_RUNTIME
    Serial.printf("lan_config ip=%u.%u.%u.%u mask=%u.%u.%u.%u gw=0.0.0.0 dns=0.0.0.0 scope=%u.%u.%u.%u\r\n",
        g_config.lan_ip[0], g_config.lan_ip[1], g_config.lan_ip[2], g_config.lan_ip[3],
        g_config.lan_mask[0], g_config.lan_mask[1], g_config.lan_mask[2], g_config.lan_mask[3],
        g_config.scope_ip[0], g_config.scope_ip[1], g_config.scope_ip[2], g_config.scope_ip[3]);
#endif

#if FW_SKIP_FY_INIT
    Serial.printf("safe bring-up: fy_init skipped, Serial2 untouched\r\n");
#elif FW_DEFER_FY_INIT
    s_release_fy_init_pending = true;
    s_release_fy_init_done = false;
    s_release_fy_init_after_ms = millis() + RELEASE_FY_INIT_DELAY_MS;
    Serial.printf("release: fy_init deferred until network/web bring-up\r\n");
#else
    fy_init();
#endif
    if (FW_ENABLE_MANUAL_FY_TEST) {
        Serial.printf("fy_manual_test=enabled serial2_initialized=%s last_manual_test=%s\r\n",
            fy_serial_initialized() ? "yes" : "no",
            fy_last_manual_test_summary());
    }
    if (FW_ENABLE_FY_SERIAL_DIAG) {
        FyDiagSnapshot diag_snapshot;
        fy_diag_get_snapshot(&diag_snapshot);
        Serial.printf("fy_serial_diag=enabled serial2_initialized=%s rx_pin=%u tx_pin=%u baud=%lu format=%s timeout=%u presence=%s\r\n",
            diag_snapshot.serial_initialized ? "yes" : "no",
            (unsigned)diag_snapshot.rx_pin,
            (unsigned)diag_snapshot.tx_pin,
            (unsigned long)diag_snapshot.baud,
            diag_snapshot.serial_mode,
            (unsigned)diag_snapshot.timeout_ms,
            fy_diag_presence_text());
    }
    if (FW_ENABLE_FY_FINAL_TEST) {
        Serial.printf("fy_final_test=enabled mode=manual_only parser=minimal_scpi sequence=safe_functional_test\r\n");
    }

    runtime_net_begin();
    scope_http_proxy_begin();
    scope_vnc_proxy_begin();
    if (runtime_net_is_ready_for_web()) {
        webconfig_begin();
    }
    scope_http_proxy_poll();
    scope_vnc_proxy_poll();
    if (runtime_net_should_run_bode()) {
        net_begin();
    }
    diag_setup();

    Serial.printf("boot_mode=%s ip=%s bode=%s web=%s\r\n",
        runtime_net_mode_text(),
        runtime_net_ip().toString().c_str(),
        net_is_running() ? "on" : (runtime_net_should_run_bode() ? "waiting_fy" : "off"),
        runtime_net_is_ready_for_web() ? "on" : "off");
    Serial.printf("boot_fail_reason=%s recovery_ap_enable=%s\r\n",
        runtime_net_last_fail_reason(),
        g_config.recovery_ap_enable ? "on" : "off");
    Serial.printf("net_snapshot lan_ip=%s lan_mask=%s lan_gw=%s lan_dns=%s sta_ip=%s sta_mode=%s ap_active=%s ap_ip=%s ntp=%s time=%s scope=%u.%u.%u.%u\r\n",
        runtime_net_lan_ip().toString().c_str(),
        runtime_net_lan_subnet().toString().c_str(),
        eth_gateway_ip().toString().c_str(),
        eth_dns_ip().toString().c_str(),
        runtime_net_sta_ip().toString().c_str(),
        g_config.use_dhcp ? "dhcp" : "static",
        runtime_net_recovery_ap_active() ? "yes" : "no",
        runtime_net_ap_ip().toString().c_str(),
        runtime_net_ntp_server_running() ? "on" : "off",
        runtime_net_time_status_text(),
        g_config.scope_ip[0], g_config.scope_ip[1], g_config.scope_ip[2], g_config.scope_ip[3]);
    Serial.printf("proxy_http supported=%s enabled=%s listening=%s port=%u target=%u.%u.%u.%u:%u active=%u last_error=%s\r\n",
        scope_http_proxy_is_supported() ? "yes" : "no",
        scope_http_proxy_is_enabled() ? "yes" : "no",
        scope_http_proxy_is_listening() ? "yes" : "no",
        (unsigned)scope_http_proxy_listen_port(),
        g_config.scope_ip[0], g_config.scope_ip[1], g_config.scope_ip[2], g_config.scope_ip[3],
        (unsigned)SCOPE_HTTP_PROXY_TARGET_PORT,
        (unsigned)scope_http_proxy_get_stats()->active_connections,
        scope_http_proxy_get_stats()->last_error);
    Serial.printf("proxy_vnc supported=%s enabled=%s listening=%s port=%u target=%u.%u.%u.%u:%u active=%u last_error=%s\r\n",
        scope_vnc_proxy_is_supported() ? "yes" : "no",
        scope_vnc_proxy_is_enabled() ? "yes" : "no",
        scope_vnc_proxy_is_listening() ? "yes" : "no",
        (unsigned)scope_vnc_proxy_listen_port(),
        g_config.scope_ip[0], g_config.scope_ip[1], g_config.scope_ip[2], g_config.scope_ip[3],
        (unsigned)SCOPE_VNC_PROXY_TARGET_PORT,
        (unsigned)scope_vnc_proxy_get_stats()->active_connections,
        scope_vnc_proxy_get_stats()->last_error);
    Serial.printf("fy_status=%s fy_enabled=%s\r\n",
        fy_status_text(),
        fy_is_enabled() ? "on" : "off");
    if (FW_ENABLE_MANUAL_FY_TEST) {
        Serial.printf("fy_manual_test_ready=yes serial2_initialized=%s last_manual_test=%s\r\n",
            fy_serial_initialized() ? "yes" : "no",
            fy_last_manual_test_summary());
    }
    if (FW_ENABLE_FY_SERIAL_DIAG) {
        FyDiagSnapshot diag_snapshot;
        fy_diag_get_snapshot(&diag_snapshot);
        Serial.printf("fy_serial_diag_ready=yes serial2_initialized=%s status=%s response=%s format=%s last_error=%s presence=%s\r\n",
            diag_snapshot.serial_initialized ? "yes" : "no",
            diag_snapshot.current_status,
            diag_snapshot.last_response_kind,
            diag_snapshot.serial_mode,
            diag_snapshot.last_error[0] ? diag_snapshot.last_error : "none",
            fy_diag_presence_text());
    }
    if (FW_ENABLE_FY_FINAL_TEST) {
        FyDiagSnapshot diag_snapshot;
        fy_diag_get_snapshot(&diag_snapshot);
        Serial.printf("fy_final_test_ready=yes last_level=%u last_parser=%s last_sequence=%s\r\n",
            (unsigned)diag_snapshot.last_functional_level,
            diag_snapshot.last_umo_parser_status[0] ? diag_snapshot.last_umo_parser_status : "not requested",
            diag_snapshot.last_sequence_result[0] ? diag_snapshot.last_sequence_result : "not requested");
    }
}

void loop()
{
    runtime_net_poll();
#if FW_DEFER_FY_INIT
    release_poll_fy_init();
#endif
    if (runtime_net_is_ready_for_web()) {
        webconfig_begin();
    }
    scope_http_proxy_poll();
    scope_vnc_proxy_poll();
    if (net_is_running()) {
        net_poll();
    }
    webconfig_poll();
    diag_poll();
    yield();
}
