#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_config.h"
#include "esp_diag_console.h"
#include "esp_eth.h"
#include "esp_fy6900.h"
#include "esp_network.h"
#include "esp_persist.h"
#include "esp_runtime_net.h"

void diag_begin_early(void)
{
    Serial.begin(115200);
    Serial.setTimeout(200);
    delay(10);
}

#if ENABLE_UART_CONSOLE

static bool s_trace_enabled = ENABLE_ETH_DIAG ? true : false;
static char s_line[128];
static uint8_t s_line_len = 0;

static void diag_print_prompt(void)
{
    Serial.print("diag> ");
}

static void diag_printf(const char *fmt, ...)
{
    char buf[192];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    Serial.print(buf);
}

bool diag_trace_enabled(void)
{
    return s_trace_enabled;
}

void diag_trace_set(bool enabled)
{
    s_trace_enabled = enabled;
}

void diag_tracef(const char *fmt, ...)
{
    if (!s_trace_enabled) return;
    char buf[192];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    Serial.print(buf);
}

static void print_ip4(const uint8_t *ip4)
{
    diag_printf("%u.%u.%u.%u", ip4[0], ip4[1], ip4[2], ip4[3]);
}

static bool parse_ip4(const char *text, uint8_t *out)
{
    unsigned a, b, c, d;
    if (sscanf(text, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return false;
    if (a > 255 || b > 255 || c > 255 || d > 255) return false;
    out[0] = (uint8_t)a;
    out[1] = (uint8_t)b;
    out[2] = (uint8_t)c;
    out[3] = (uint8_t)d;
    return true;
}

static void cmd_help(void)
{
    diag_printf("help\r\n");
    diag_printf("status\r\n");
    diag_printf("eth\r\n");
    diag_printf("cfg show | cfg reset | cfg dhcp on|off | cfg ip <ip> <mask> <gw> <dns>\r\n");
    diag_printf("reboot\r\n");
    diag_printf("rpc | vxi | sessions\r\n");
    diag_printf("awg enable | awg disable | awg raw <command> | awg poll | awg baud <value>\r\n");
    diag_printf("fy summary | fy init | fy reinit | fy deinit | fy flush\r\n");
    diag_printf("fy read [timeout_ms] | fy probe [timeout_ms] | fy compare [timeout_ms] | fy functional [timeout_ms] | fy resetstate | fy send <command>\r\n");
    diag_printf("trace on | trace off\r\n");
}

static void cmd_status(void)
{
    const NetStats *stats = net_get_stats();

    diag_printf("firmware=%s build=%s\r\n", FW_VARIANT_NAME, FW_BUILD_STRING);
    diag_printf("runtime mode=%s web=%s bode=%s fail=%s\r\n",
        runtime_net_mode_text(),
        runtime_net_is_ready_for_web() ? "ready" : "offline",
        runtime_net_should_run_bode() ? "ready" : "off",
        runtime_net_last_fail_reason());
    diag_printf("lan link=%s connected=%s ip=%s mask=%s ntp=%s served=%lu\r\n",
        runtime_net_lan_link_up() ? "up" : "down",
        runtime_net_lan_has_ip() ? "yes" : "no",
        runtime_net_lan_ip().toString().c_str(),
        runtime_net_lan_subnet().toString().c_str(),
        runtime_net_ntp_server_running() ? runtime_net_time_status_text() : "disabled",
        (unsigned long)runtime_net_ntp_request_count());
    diag_printf("ntp upstream=%s\r\n", g_config.ntp_server);
    diag_printf("wifi sta_valid=%s connecting=%s connected=%s ssid=%s ip=%s ap=%s\r\n",
        runtime_net_sta_config_valid() ? "yes" : "no",
        runtime_net_sta_connecting() ? "yes" : "no",
        runtime_net_sta_connected() ? "yes" : "no",
        runtime_net_ssid()[0] ? runtime_net_ssid() : g_config.wifi_ssid,
        runtime_net_sta_ip().toString().c_str(),
        runtime_net_recovery_ap_active() ? "on" : "off");
    diag_printf("wifi retry=%u/%u exhausted=%s ap_ssid=%s ap_security=%s\r\n",
        (unsigned)runtime_net_sta_retry_count(),
        (unsigned)runtime_net_sta_retry_limit(),
        runtime_net_sta_retry_exhausted() ? "yes" : "no",
        g_config.ap_ssid,
        g_config.ap_password[0] ? "secured" : "open");
    diag_printf("eth mode=%s link=%s connected=%s ip=%s fail=%s\r\n",
        eth_mode_text(),
        eth_link_is_up() ? "up" : "down",
        eth_is_connected() ? "yes" : "no",
        eth_current_ip().toString().c_str(),
        eth_last_fail_reason_text());
    diag_printf("cfg stored_valid=%s current_crc=%s dhcp=%s hostname=%s\r\n",
        config_store_was_valid() ? "yes" : "no",
        config_current_is_valid() ? "yes" : "no",
        g_config.use_dhcp ? "on" : "off",
        g_config.device_hostname);
    diag_printf("awg enabled=%s baud=%lu trace=%s\r\n",
        fy_is_enabled() ? "yes" : "no",
        (unsigned long)fy_get_baud(),
        s_trace_enabled ? "on" : "off");
    diag_printf("vxi port=%u session=%s last=%s\r\n",
        stats->current_vxi_port,
        stats->session_active ? "active" : "idle",
        stats->last_event);
}

static void cmd_eth(void)
{
    diag_printf("link=%s connected=%s fail=%s mac=%s\r\n",
        eth_link_is_up() ? "up" : "down",
        eth_is_connected() ? "yes" : "no",
        eth_last_fail_reason_text(),
        eth_mac_string());
    diag_printf("ip=%s gw=%s dns=%s subnet=%s speed=%u duplex=%s\r\n",
        eth_current_ip().toString().c_str(),
        eth_gateway_ip().toString().c_str(),
        eth_dns_ip().toString().c_str(),
        eth_subnet_mask().toString().c_str(),
        (unsigned)eth_link_speed_mbps(),
        eth_is_full_duplex() ? "full" : "half");
    diag_printf("runtime_lan_ip=%s runtime_lan_mask=%s ntp_server=%s time=%s requests=%lu\r\n",
        runtime_net_lan_ip().toString().c_str(),
        runtime_net_lan_subnet().toString().c_str(),
        runtime_net_ntp_server_running() ? "on" : "off",
        runtime_net_time_status_text(),
        (unsigned long)runtime_net_ntp_request_count());
}

static void cmd_cfg_show(void)
{
    diag_printf("stored_valid=%s current_crc=%s version=%u\r\n",
        config_store_was_valid() ? "yes" : "no",
        config_current_is_valid() ? "yes" : "no",
        g_config.version);
    diag_printf("hostname=%s friendly=%s idn=%s dhcp=%s\r\n",
        g_config.device_hostname,
        g_config.friendly_name,
        g_config.idn_response_name,
        g_config.use_dhcp ? "on" : "off");
    diag_printf("ntp_host=%s\r\n", g_config.ntp_server);
    diag_printf("ip="); print_ip4(g_config.ip);
    diag_printf(" mask="); print_ip4(g_config.mask);
    diag_printf(" gw="); print_ip4(g_config.gw);
    diag_printf(" dns="); print_ip4(g_config.dns);
    diag_printf("\r\n");
    diag_printf("awg_baud=%lu family=%u\r\n",
        (unsigned long)g_config.awg_baud,
        (unsigned)g_config.awg_firmware_family);
}

static void cmd_cfg_reset(void)
{
    diag_printf("restoring defaults and rebooting\r\n");
    config_reset_defaults();
    config_save();
    delay(200);
    ESP.restart();
}

static void cmd_cfg_dhcp(bool enabled)
{
    g_config.use_dhcp = enabled ? 1 : 0;
    config_save();
    diag_printf("dhcp=%s saved; reboot to reapply\r\n", enabled ? "on" : "off");
}

static void cmd_cfg_ip(char *args)
{
    char ip[20], mask[20], gw[20], dns[20];
    if (sscanf(args, "%19s %19s %19s %19s", ip, mask, gw, dns) != 4) {
        diag_printf("usage: cfg ip <ip> <mask> <gw> <dns>\r\n");
        return;
    }
    if (!parse_ip4(ip, g_config.ip) || !parse_ip4(mask, g_config.mask)
            || !parse_ip4(gw, g_config.gw) || !parse_ip4(dns, g_config.dns)) {
        diag_printf("invalid ip tuple\r\n");
        return;
    }
    config_save();
    diag_printf("static ip tuple saved; reboot to reapply\r\n");
}

static void cmd_rpc(void)
{
    const NetStats *stats = net_get_stats();
    diag_printf("udp_getport=%lu tcp_getport=%lu zero_reply=%lu\r\n",
        (unsigned long)stats->udp_getport_count,
        (unsigned long)stats->tcp_getport_count,
        (unsigned long)stats->getport_zero_reply_count);
    diag_printf("create=%lu write=%lu read=%lu destroy=%lu last=%s\r\n",
        (unsigned long)stats->create_link_count,
        (unsigned long)stats->device_write_count,
        (unsigned long)stats->device_read_count,
        (unsigned long)stats->destroy_link_count,
        stats->last_event);
}

static void cmd_vxi(void)
{
    const NetStats *stats = net_get_stats();
    diag_printf("current_vxi_port=%u active=%s client=%s:%u\r\n",
        stats->current_vxi_port,
        stats->session_active ? "yes" : "no",
        stats->active_client_ip.toString().c_str(),
        stats->active_client_port);
    diag_printf("last_write=%lu last_read=%lu last=%s\r\n",
        (unsigned long)stats->last_write_len,
        (unsigned long)stats->last_read_len,
        stats->last_event);
}

static void cmd_sessions(void)
{
    const NetStats *stats = net_get_stats();
    diag_printf("accepted=%lu ended=%lu dropped=%lu active=%s\r\n",
        (unsigned long)stats->session_accept_count,
        (unsigned long)stats->session_end_count,
        (unsigned long)stats->session_drop_count,
        stats->session_active ? "yes" : "no");
    diag_printf("client=%s:%u current_vxi_port=%u\r\n",
        stats->active_client_ip.toString().c_str(),
        stats->active_client_port,
        stats->current_vxi_port);
}

static void cmd_awg_raw(const char *cmd)
{
    char reply[96];
    int n;
    if (!fy_is_enabled()) {
        diag_printf("awg disabled\r\n");
        return;
    }
    diag_printf("raw tx: %s\r\n", cmd);
    n = fy_raw_cmd(cmd, reply, sizeof(reply));
    if (n < 0) {
        diag_printf("raw rejected\r\n");
        return;
    }
    diag_printf("raw rx (%d): %s\r\n", n, reply);
}

static void cmd_awg_poll(void)
{
    char buf[96];
    int n = fy_read_available(buf, sizeof(buf));
    diag_printf("awg poll bytes=%d\r\n", n);
    if (n > 0) diag_printf("%s\r\n", buf);
}

static void cmd_fy_summary(void)
{
    FyDiagSnapshot diag;
    fy_diag_get_snapshot(&diag);
    diag_printf("fy summary serial2=%s presence=%s status=%s response=%s baud=%lu format=%s timeout=%u pins rx=%u tx=%u\r\n",
        diag.serial_initialized ? "yes" : "no",
        fy_diag_presence_text(),
        diag.current_status,
        diag.last_response_kind,
        (unsigned long)diag.baud,
        diag.serial_mode,
        (unsigned)diag.timeout_ms,
        (unsigned)diag.rx_pin,
        (unsigned)diag.tx_pin);
    diag_printf("fy counters tx=%lu rx=%lu timeout=%lu invalid=%lu op=%s #%lu @ %lu ms\r\n",
        (unsigned long)diag.tx_total_bytes,
        (unsigned long)diag.rx_total_bytes,
        (unsigned long)diag.timeout_count,
        (unsigned long)diag.invalid_response_count,
        diag.last_operation,
        (unsigned long)diag.operation_count,
        (unsigned long)diag.last_operation_ms);
    diag_printf("fy last_error=%s\r\n", diag.last_error[0] ? diag.last_error : "none");
    diag_printf("fy last_tx=%s\r\n", diag.last_tx_raw[0] ? diag.last_tx_raw : "(empty)");
    diag_printf("fy last_rx_ascii=%s\r\n", diag.last_rx_ascii[0] ? diag.last_rx_ascii : "(empty)");
    diag_printf("fy last_rx_hex=%s\r\n", diag.last_rx_hex[0] ? diag.last_rx_hex : "(empty)");
    diag_printf("fy umo_raw=%s\r\n", diag.last_umo_raw[0] ? diag.last_umo_raw : "(empty)");
    diag_printf("fy umo_parsed=%s parser=%s\r\n",
        diag.last_umo_parsed[0] ? diag.last_umo_parsed : "(empty)",
        diag.last_umo_parser_status[0] ? diag.last_umo_parser_status : "not requested");
    diag_printf("fy functional level=%u success=%s\r\n",
        (unsigned)diag.last_functional_level,
        diag.last_functional_success ? "yes" : "no");
    diag_printf("fy parser_readback=%s\r\n", diag.last_parser_readback[0] ? diag.last_parser_readback : "(empty)");
    diag_printf("fy sequence=%s\r\n", diag.last_sequence_result[0] ? diag.last_sequence_result : "not requested");
}

static void cmd_fy_init(void)
{
    bool ok = fy_diag_uart_init();
    diag_printf("fy init serial2=%s\r\n", ok ? "yes" : "no");
}

static void cmd_fy_reinit(void)
{
    bool ok = fy_diag_uart_reinit();
    diag_printf("fy reinit serial2=%s\r\n", ok ? "yes" : "no");
}

static void cmd_fy_deinit(void)
{
    fy_diag_uart_deinit();
    diag_printf("fy deinit serial2=no\r\n");
}

static void cmd_fy_flush(void)
{
    size_t count = fy_diag_flush_rx();
    diag_printf("fy flush bytes=%u\r\n", (unsigned)count);
}

static void cmd_fy_read(char *args)
{
    uint16_t timeout_ms = 0;
    if (args != NULL) {
        while (*args == ' ' || *args == '\t') args++;
        if (*args != '\0') {
            timeout_ms = (uint16_t)strtoul(args, NULL, 10);
        }
    }
    diag_printf("fy read timeout=%u rx=%d\r\n",
        (unsigned)timeout_ms,
        fy_diag_read_raw(timeout_ms));
}

static void cmd_fy_probe(char *args)
{
    uint16_t timeout_ms = 0;
    char summary[192];
    if (args != NULL) {
        while (*args == ' ' || *args == '\t') args++;
        if (*args != '\0') {
            timeout_ms = (uint16_t)strtoul(args, NULL, 10);
        }
    }
    bool ok = fy_diag_probe_protocol(timeout_ms, summary, sizeof(summary));
    diag_printf("fy probe result=%s summary=%s\r\n", ok ? "success" : "fail", summary);
}

static void cmd_fy_compare(char *args)
{
    uint16_t timeout_ms = 0;
    char summary[256];
    if (args != NULL) {
        while (*args == ' ' || *args == '\t') args++;
        if (*args != '\0') {
            timeout_ms = (uint16_t)strtoul(args, NULL, 10);
        }
    }
    bool ok = fy_diag_compare_8n2_vs_8n1(timeout_ms, summary, sizeof(summary));
    diag_printf("fy compare result=%s summary=%s\r\n", ok ? "success" : "fail", summary);
}

static void cmd_fy_functional(char *args)
{
    uint16_t timeout_ms = 0;
    char summary[224];
    if (args != NULL) {
        while (*args == ' ' || *args == '\t') args++;
        if (*args != '\0') {
            timeout_ms = (uint16_t)strtoul(args, NULL, 10);
        }
    }
    bool ok = fy_functional_test_run(timeout_ms, summary, sizeof(summary));
    diag_printf("fy functional result=%s summary=%s\r\n", ok ? "success" : "fail", summary);
}

static void cmd_fy_resetstate(void)
{
    fy_functional_reset_state();
    diag_printf("fy resetstate done\r\n");
}

static void cmd_fy_send(char *args)
{
    while (*args == ' ' || *args == '\t') args++;
    if (*args == '\0') {
        diag_printf("usage: fy send <command>\r\n");
        return;
    }
    diag_printf("fy send rx=%d\r\n",
        fy_diag_send_raw(args, true, fy_get_timeout_ms()));
}

static void process_line(char *line)
{
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '\0') {
        diag_print_prompt();
        return;
    }

    if (strcmp(line, "help") == 0) cmd_help();
    else if (strcmp(line, "status") == 0) cmd_status();
    else if (strcmp(line, "eth") == 0) cmd_eth();
    else if (strcmp(line, "cfg show") == 0) cmd_cfg_show();
    else if (strcmp(line, "cfg reset") == 0) cmd_cfg_reset();
    else if (strcmp(line, "cfg dhcp on") == 0) cmd_cfg_dhcp(true);
    else if (strcmp(line, "cfg dhcp off") == 0) cmd_cfg_dhcp(false);
    else if (strncmp(line, "cfg ip ", 7) == 0) cmd_cfg_ip(line + 7);
    else if (strcmp(line, "reboot") == 0) { diag_printf("rebooting\r\n"); delay(100); ESP.restart(); }
    else if (strcmp(line, "rpc") == 0) cmd_rpc();
    else if (strcmp(line, "vxi") == 0) cmd_vxi();
    else if (strcmp(line, "sessions") == 0) cmd_sessions();
    else if (strcmp(line, "awg enable") == 0) { fy_set_enabled(true); diag_printf("awg enabled\r\n"); }
    else if (strcmp(line, "awg disable") == 0) { fy_set_enabled(false); diag_printf("awg disabled\r\n"); }
    else if (strncmp(line, "awg raw ", 8) == 0) cmd_awg_raw(line + 8);
    else if (strcmp(line, "awg poll") == 0) cmd_awg_poll();
    else if (strncmp(line, "awg baud ", 9) == 0) {
        unsigned long baud = strtoul(line + 9, NULL, 10);
        if (!fy_is_supported_baud((uint32_t)baud)) {
            diag_printf("unsupported baud (9600,19200,38400,57600,115200)\r\n");
        } else {
            fy_set_baud((uint32_t)baud);
            diag_printf("awg serial2 baud=%lu\r\n", baud);
        }
    }
    else if (strcmp(line, "fy summary") == 0) cmd_fy_summary();
    else if (strcmp(line, "fy init") == 0) cmd_fy_init();
    else if (strcmp(line, "fy reinit") == 0) cmd_fy_reinit();
    else if (strcmp(line, "fy deinit") == 0) cmd_fy_deinit();
    else if (strcmp(line, "fy flush") == 0) cmd_fy_flush();
    else if (strncmp(line, "fy read", 7) == 0) cmd_fy_read(line + 7);
    else if (strncmp(line, "fy probe", 8) == 0) cmd_fy_probe(line + 8);
    else if (strncmp(line, "fy compare", 10) == 0) cmd_fy_compare(line + 10);
    else if (strncmp(line, "fy functional", 13) == 0) cmd_fy_functional(line + 13);
    else if (strcmp(line, "fy resetstate") == 0) cmd_fy_resetstate();
    else if (strncmp(line, "fy send ", 8) == 0) cmd_fy_send(line + 8);
    else if (strcmp(line, "trace on") == 0) { diag_trace_set(true); diag_printf("trace on\r\n"); }
    else if (strcmp(line, "trace off") == 0) { diag_trace_set(false); diag_printf("trace off\r\n"); }
    else diag_printf("unknown command\r\n");

    diag_print_prompt();
}

void diag_setup(void)
{
    diag_printf("\r\nespBode %s\r\n", FW_VARIANT_NAME);
    diag_printf("build: %s\r\n", FW_BUILD_STRING);
    diag_printf("stored config valid: %s\r\n", config_store_was_valid() ? "yes" : "no");
    diag_printf("eth: link=%s connected=%s ip=%s mac=%s\r\n",
        eth_link_is_up() ? "up" : "down",
        eth_is_connected() ? "yes" : "no",
        eth_current_ip().toString().c_str(),
        eth_mac_string());
    diag_printf("awg: enabled=%s baud=%lu\r\n",
        fy_is_enabled() ? "yes" : "no",
        (unsigned long)fy_get_baud());
    diag_print_prompt();
}

void diag_poll(void)
{
    while (Serial.available()) {
        char ch = (char)Serial.read();
        if (ch == '\r' || ch == '\n') {
            s_line[s_line_len] = '\0';
            Serial.print("\r\n");
            process_line(s_line);
            s_line_len = 0;
            continue;
        }
        if ((ch == '\b' || ch == 0x7F) && s_line_len > 0) {
            s_line_len--;
            continue;
        }
        if (s_line_len < sizeof(s_line) - 1 && ch >= 32 && ch < 127) {
            s_line[s_line_len++] = ch;
        }
    }
}

#else

void diag_setup(void) {}
void diag_poll(void) {}
bool diag_trace_enabled(void) { return false; }
void diag_trace_set(bool enabled) { (void)enabled; }
void diag_tracef(const char *fmt, ...) { (void)fmt; }

#endif
