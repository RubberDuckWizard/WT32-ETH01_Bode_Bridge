#include "esp_eth.h"
#include "esp_config.h"

#if ENABLE_ETH_RUNTIME

#include <Arduino.h>
#include <ETH.h>
#include <WiFi.h>

#include "esp_diag_console.h"
#include "esp_persist.h"

static bool s_event_handler_registered = false;
static bool s_eth_started = false;
static bool s_eth_link_up = false;
static bool s_eth_has_ip = false;
static bool s_eth_full_duplex = false;
static uint8_t s_eth_link_speed = 0;
static EthFailReason s_last_fail_reason = ETH_FAIL_NONE;
static String s_eth_mac;
static bool s_eth_inferred_ip_logged = false;

static bool ip_is_zero(IPAddress ip)
{
    return ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] == 0;
}

static IPAddress lan_ip_addr(void)
{
    return IPAddress(g_config.lan_ip[0], g_config.lan_ip[1], g_config.lan_ip[2], g_config.lan_ip[3]);
}

static IPAddress lan_mask_addr(void)
{
    return IPAddress(g_config.lan_mask[0], g_config.lan_mask[1], g_config.lan_mask[2], g_config.lan_mask[3]);
}

static void refresh_eth_ip_state(void)
{
    bool has_driver_ip;

    if (!s_eth_started) {
        s_eth_has_ip = false;
        s_eth_inferred_ip_logged = false;
        return;
    }

    has_driver_ip = !ip_is_zero(ETH.localIP());
    s_eth_has_ip = has_driver_ip;

    if (s_eth_link_up && has_driver_ip && !s_eth_inferred_ip_logged) {
        s_eth_inferred_ip_logged = true;
        Serial.printf("[eth] runtime ip=%s mask=%s gw=%s dns=%s\r\n",
            ETH.localIP().toString().c_str(),
            ETH.subnetMask().toString().c_str(),
            ETH.gatewayIP().toString().c_str(),
            ETH.dnsIP().toString().c_str());
    }
}

static const char *cfg_hostname(void)
{
    return g_config.device_hostname[0] ? g_config.device_hostname : DEF_DEVICE_HOSTNAME;
}

static const char *reason_text(EthFailReason reason)
{
    switch (reason) {
    case ETH_FAIL_NONE:             return "none";
    case ETH_FAIL_BEGIN:            return "begin_failed";
    case ETH_FAIL_LINK_TIMEOUT:     return "link_timeout";
    case ETH_FAIL_IP_TIMEOUT:       return "ip_timeout";
    case ETH_FAIL_STATIC_IP_FAILED: return "static_ip_failed";
    default:                        return "unknown";
    }
}

static void handle_eth_event(WiFiEvent_t event)
{
    switch (event) {
    case ARDUINO_EVENT_ETH_START:
        s_eth_started = true;
        s_eth_link_up = false;
        s_eth_has_ip = false;
        s_eth_full_duplex = false;
        s_eth_link_speed = 0;
        ETH.setHostname(cfg_hostname());
        Serial.printf("[eth] start hostname=%s\r\n", cfg_hostname());
        diag_tracef("[eth] start hostname=%s\r\n", cfg_hostname());
        break;

    case ARDUINO_EVENT_ETH_CONNECTED:
        s_eth_link_up = true;
        Serial.printf("[eth] link up\r\n");
        diag_tracef("[eth] link up\r\n");
        break;

    case ARDUINO_EVENT_ETH_GOT_IP:
        s_eth_started = true;
        s_eth_link_up = true;
        s_eth_has_ip = !ip_is_zero(ETH.localIP());
        s_eth_inferred_ip_logged = true;
        s_eth_full_duplex = ETH.fullDuplex();
        s_eth_link_speed = ETH.linkSpeed();
        s_eth_mac = ETH.macAddress();
        s_last_fail_reason = ETH_FAIL_NONE;
        Serial.printf("[eth] got ip=%s mask=%s gw=%s dns=%s mac=%s speed=%u duplex=%s\r\n",
            ETH.localIP().toString().c_str(),
            ETH.subnetMask().toString().c_str(),
            ETH.gatewayIP().toString().c_str(),
            ETH.dnsIP().toString().c_str(),
            s_eth_mac.c_str(),
            (unsigned)s_eth_link_speed,
            s_eth_full_duplex ? "full" : "half");
        diag_tracef("[eth] got ip=%s gw=%s dns=%s mac=%s speed=%u duplex=%s\r\n",
            ETH.localIP().toString().c_str(),
            ETH.gatewayIP().toString().c_str(),
            ETH.dnsIP().toString().c_str(),
            s_eth_mac.c_str(),
            (unsigned)s_eth_link_speed,
            s_eth_full_duplex ? "full" : "half");
        break;

    case ARDUINO_EVENT_ETH_DISCONNECTED:
        s_eth_link_up = false;
        s_eth_has_ip = false;
        s_eth_full_duplex = false;
        s_eth_link_speed = 0;
        Serial.printf("[eth] disconnected\r\n");
        diag_tracef("[eth] disconnected\r\n");
        break;

    case ARDUINO_EVENT_ETH_STOP:
        s_eth_started = false;
        s_eth_link_up = false;
        s_eth_has_ip = false;
        s_eth_full_duplex = false;
        s_eth_link_speed = 0;
        Serial.printf("[eth] stopped\r\n");
        diag_tracef("[eth] stopped\r\n");
        break;

    default:
        break;
    }
}

static bool apply_static_ip_config(void)
{
    IPAddress ip = lan_ip_addr();
    IPAddress mask = lan_mask_addr();
    IPAddress zero((uint32_t)0);

    for (int attempt = 0; attempt < 20; ++attempt) {
        if (ETH.config(ip, zero, mask, zero, zero)) {
            return true;
        }
        delay(50);
        yield();
    }
    return false;
}

bool eth_begin_runtime(void)
{
    uint32_t t0;
    IPAddress ip = lan_ip_addr();
    IPAddress mask = lan_mask_addr();

    s_last_fail_reason = ETH_FAIL_NONE;
    s_eth_started = false;
    s_eth_link_up = false;
    s_eth_has_ip = false;
    s_eth_full_duplex = false;
    s_eth_link_speed = 0;
    s_eth_mac = "";

    if (!s_event_handler_registered) {
        WiFi.onEvent(handle_eth_event);
        s_event_handler_registered = true;
    }

    WiFi.persistent(false);
    WiFi.mode(WIFI_MODE_NULL);
    delay(20);

    Serial.printf("[eth] begin ip=%s mask=%s gw=0.0.0.0 dns=0.0.0.0 host=%s\r\n",
        ip.toString().c_str(),
        mask.toString().c_str(),
        cfg_hostname());

    if (!ETH.begin(ETH_PHY_ADDR, ETH_PHY_POWER, ETH_PHY_MDC, ETH_PHY_MDIO,
            ETH_PHY_TYPE, ETH_CLK_MODE)) {
        s_last_fail_reason = ETH_FAIL_BEGIN;
        Serial.printf("[eth] begin failed\r\n");
        diag_tracef("[eth] begin failed\r\n");
        return false;
    }

    t0 = millis();
    while (!s_eth_started && (uint32_t)(millis() - t0) < 1000U) {
        delay(10);
        yield();
    }

    if (!apply_static_ip_config()) {
        s_last_fail_reason = ETH_FAIL_STATIC_IP_FAILED;
        Serial.printf("[eth] static ip apply failed\r\n");
        diag_tracef("[eth] static ip apply failed\r\n");
        return false;
    }

    ETH.setHostname(cfg_hostname());

    return true;
}

bool eth_setup(void)
{
    uint32_t t0;

    if (!eth_begin_runtime()) {
        return false;
    }

    t0 = millis();
    while (!s_eth_link_up) {
        if ((uint32_t)(millis() - t0) > ETH_LINK_TIMEOUT_MS) {
            s_last_fail_reason = ETH_FAIL_LINK_TIMEOUT;
            diag_tracef("[eth] link timeout after %lu ms\r\n", (unsigned long)(millis() - t0));
            return false;
        }
        delay(50);
        yield();
    }

    t0 = millis();
    while (!s_eth_has_ip || ip_is_zero(ETH.localIP())) {
        if ((uint32_t)(millis() - t0) > ETH_IP_TIMEOUT_MS) {
            s_last_fail_reason = ETH_FAIL_IP_TIMEOUT;
            diag_tracef("[eth] ip timeout after %lu ms\r\n", (unsigned long)(millis() - t0));
            return false;
        }
        delay(50);
        yield();
    }

    s_last_fail_reason = ETH_FAIL_NONE;
    return true;
}

bool eth_is_connected(void)
{
    refresh_eth_ip_state();
    return s_eth_link_up && s_eth_has_ip;
}

bool eth_started(void)
{
    return s_eth_started;
}

bool eth_link_is_up(void)
{
    return s_eth_link_up;
}

EthFailReason eth_last_fail_reason(void)
{
    return s_last_fail_reason;
}

const char *eth_last_fail_reason_text(void)
{
    return reason_text(s_last_fail_reason);
}

const char *eth_mode_text(void)
{
    return "Ethernet";
}

IPAddress eth_current_ip(void)
{
    refresh_eth_ip_state();
    return s_eth_started ? ETH.localIP() : IPAddress((uint32_t)0);
}

IPAddress eth_gateway_ip(void)
{
    refresh_eth_ip_state();
    return s_eth_started ? ETH.gatewayIP() : IPAddress((uint32_t)0);
}

IPAddress eth_subnet_mask(void)
{
    refresh_eth_ip_state();
    return s_eth_started ? ETH.subnetMask() : IPAddress((uint32_t)0);
}

IPAddress eth_dns_ip(void)
{
    refresh_eth_ip_state();
    return s_eth_started ? ETH.dnsIP() : IPAddress((uint32_t)0);
}

const char *eth_mac_string(void)
{
    if (s_eth_mac.length() == 0 && s_eth_started) {
        s_eth_mac = ETH.macAddress();
    }
    return s_eth_mac.c_str();
}

uint8_t eth_link_speed_mbps(void)
{
    return s_eth_link_speed;
}

bool eth_is_full_duplex(void)
{
    return s_eth_full_duplex;
}

#else

bool eth_setup(void)
{
    return false;
}

bool eth_begin_runtime(void)
{
    return false;
}

bool eth_is_connected(void)
{
    return false;
}

bool eth_started(void)
{
    return false;
}

bool eth_link_is_up(void)
{
    return false;
}

EthFailReason eth_last_fail_reason(void)
{
    return ETH_FAIL_NONE;
}

const char *eth_last_fail_reason_text(void)
{
    return "disabled";
}

const char *eth_mode_text(void)
{
    return "disabled";
}

IPAddress eth_current_ip(void)
{
    return IPAddress((uint32_t)0);
}

IPAddress eth_gateway_ip(void)
{
    return IPAddress((uint32_t)0);
}

IPAddress eth_subnet_mask(void)
{
    return IPAddress((uint32_t)0);
}

IPAddress eth_dns_ip(void)
{
    return IPAddress((uint32_t)0);
}

const char *eth_mac_string(void)
{
    return "";
}

uint8_t eth_link_speed_mbps(void)
{
    return 0u;
}

bool eth_is_full_duplex(void)
{
    return false;
}

#endif
