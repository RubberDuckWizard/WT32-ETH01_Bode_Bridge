#include <WebServer.h>
#include <ctype.h>
#include <esp_system.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_config.h"
#include "esp_eth.h"
#include "esp_fy6900.h"
#include "esp_network.h"
#include "esp_persist.h"
#include "esp_runtime_net.h"
#include "esp_scope_http_proxy.h"
#include "esp_scope_vnc_proxy.h"
#include "esp_webconfig.h"

namespace {

struct WebUiPendingClient {
    bool active;
    WiFiClient client;
    IPAddress remote_ip;
    uint16_t remote_port;
    uint32_t accepted_ms;
};

class LimitedWebServer : public WebServer {
public:
    explicit LimitedWebServer(int port)
        : WebServer(port)
    {
        memset(&m_stats, 0, sizeof(m_stats));
        clear_slots();
        set_last_error("none");
        m_stats.listen_port = (uint16_t)port;
    }

    void begin() override
    {
        WebServer::begin();
        m_stats.listening = true;
        update_stats();
    }

    void poll_limited()
    {
        if (!m_stats.listening) {
            return;
        }
        accept_pending_clients();
        process_pending_clients();
        update_stats();
    }

    const WebUiServerStats *stats()
    {
        update_stats();
        return &m_stats;
    }

private:
    WebUiPendingClient m_slots[WEB_UI_MAX_CONNECTION_SLOTS];
    WebUiServerStats m_stats;

    static uint8_t configured_max_clients()
    {
        uint8_t value = g_config.max_web_ui_clients;

        if (value < MIN_WEB_SERVICE_CLIENTS) {
            value = MIN_WEB_SERVICE_CLIENTS;
        }
        if (value > WEB_UI_MAX_CONNECTION_SLOTS) {
            value = WEB_UI_MAX_CONNECTION_SLOTS;
        }
        return value;
    }

    void clear_slots()
    {
        for (size_t i = 0; i < WEB_UI_MAX_CONNECTION_SLOTS; ++i) {
            clear_slot(m_slots[i]);
        }
    }

    void clear_slot(WebUiPendingClient &slot)
    {
        if (slot.client) {
            slot.client.stop();
        }
        slot.active = false;
        slot.client = WiFiClient();
        slot.remote_ip = IPAddress();
        slot.remote_port = 0u;
        slot.accepted_ms = 0u;
    }

    void set_last_error(const char *text)
    {
        if (text == NULL) {
            text = "unknown";
        }
        strncpy(m_stats.last_error, text, sizeof(m_stats.last_error) - 1u);
        m_stats.last_error[sizeof(m_stats.last_error) - 1u] = '\0';
    }

    uint8_t active_client_count() const
    {
        uint8_t active = 0u;

        for (size_t i = 0; i < WEB_UI_MAX_CONNECTION_SLOTS; ++i) {
            if (m_slots[i].active) {
                ++active;
            }
        }
        return active;
    }

    int find_free_slot() const
    {
        for (size_t i = 0; i < WEB_UI_MAX_CONNECTION_SLOTS; ++i) {
            if (!m_slots[i].active) {
                return (int)i;
            }
        }
        return -1;
    }

    void update_stats()
    {
        m_stats.listen_port = WEB_UI_LISTEN_PORT;
        m_stats.max_clients = configured_max_clients();
        m_stats.active_clients = active_client_count();
    }

    static void write_service_unavailable(WiFiClient &client)
    {
        client.setNoDelay(true);
        (void)client.print(
            "HTTP/1.1 503 Service Unavailable\r\n"
            "Connection: close\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 19\r\n"
            "\r\n"
            "Service Unavailable");
    }

    void reject_client(WiFiClient &incoming, const char *reason)
    {
        write_service_unavailable(incoming);
        incoming.stop();
        ++m_stats.rejected_connections;
        set_last_error(reason != NULL ? reason : "limit_reached");
        Serial.printf("[webui] reject client=%s:%u reason=%s active=%u max=%u\r\n",
            incoming.remoteIP().toString().c_str(),
            (unsigned)incoming.remotePort(),
            reason != NULL ? reason : "limit_reached",
            (unsigned)active_client_count(),
            (unsigned)configured_max_clients());
    }

    void accept_pending_clients()
    {
        for (;;) {
            WiFiClient incoming = _server.available();
            int slot_index;

            if (!incoming) {
                break;
            }

            slot_index = find_free_slot();
            if (slot_index < 0 || active_client_count() >= configured_max_clients()) {
                reject_client(incoming, "limit_reached");
                continue;
            }

            incoming.setNoDelay(true);
            m_slots[slot_index].active = true;
            m_slots[slot_index].client = incoming;
            m_slots[slot_index].remote_ip = incoming.remoteIP();
            m_slots[slot_index].remote_port = incoming.remotePort();
            m_slots[slot_index].accepted_ms = millis();
            ++m_stats.accepted_connections;
            set_last_error("none");
            Serial.printf("[webui] accept slot=%d client=%s:%u active=%u max=%u\r\n",
                slot_index,
                incoming.remoteIP().toString().c_str(),
                (unsigned)incoming.remotePort(),
                (unsigned)(active_client_count()),
                (unsigned)configured_max_clients());
        }
    }

    void finalize_current_client()
    {
        _currentClient = WiFiClient();
        _currentStatus = HC_NONE;
        _currentUpload.reset();
        _currentRaw.reset();
    }

    void process_pending_clients()
    {
        for (size_t i = 0; i < WEB_UI_MAX_CONNECTION_SLOTS; ++i) {
            WebUiPendingClient &slot = m_slots[i];
            bool client_open;

            if (!slot.active) {
                continue;
            }

            client_open = slot.client.connected() || slot.client.available() > 0;
            if (!client_open) {
                clear_slot(slot);
                continue;
            }
            if (slot.client.available() <= 0) {
                if ((uint32_t)(millis() - slot.accepted_ms) > HTTP_MAX_DATA_WAIT) {
                    Serial.printf("[webui] close slot=%u client=%s:%u reason=read_timeout\r\n",
                        (unsigned)i,
                        slot.remote_ip.toString().c_str(),
                        (unsigned)slot.remote_port);
                    clear_slot(slot);
                }
                continue;
            }

            _currentClient = slot.client;
            _currentStatus = HC_WAIT_READ;
            _statusChange = slot.accepted_ms;
            if (_parseRequest(_currentClient)) {
                _currentClient.setTimeout(HTTP_MAX_SEND_WAIT / 1000);
                _contentLength = CONTENT_LENGTH_NOT_SET;
                _handleRequest();
                ++m_stats.completed_connections;
                set_last_error("none");
            } else {
                set_last_error("parse_failed");
                Serial.printf("[webui] close slot=%u client=%s:%u reason=parse_failed\r\n",
                    (unsigned)i,
                    slot.remote_ip.toString().c_str(),
                    (unsigned)slot.remote_port);
            }
            slot.client.stop();
            finalize_current_client();
            clear_slot(slot);
        }
    }
};

}  // namespace

static LimitedWebServer s_server(WEB_UI_LISTEN_PORT);
static bool s_server_started = false;

static String ip_to_str(const uint8_t *ip4)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", ip4[0], ip4[1], ip4[2], ip4[3]);
    return String(buf);
}

static String ip_to_str_or_empty(const uint8_t *ip4)
{
    if (ip4[0] == 0 && ip4[1] == 0 && ip4[2] == 0 && ip4[3] == 0) {
        return String("");
    }
    return ip_to_str(ip4);
}

static bool str_to_ip(const String &text, uint8_t *ip4)
{
    unsigned a, b, c, d;
    if (sscanf(text.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return false;
    if (a > 255 || b > 255 || c > 255 || d > 255) return false;
    ip4[0] = (uint8_t)a;
    ip4[1] = (uint8_t)b;
    ip4[2] = (uint8_t)c;
    ip4[3] = (uint8_t)d;
    return true;
}

static uint32_t ip_to_u32(const uint8_t *ip4)
{
    return ((uint32_t)ip4[0] << 24)
        | ((uint32_t)ip4[1] << 16)
        | ((uint32_t)ip4[2] << 8)
        | (uint32_t)ip4[3];
}

static bool is_zero_ip(const uint8_t *ip4)
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

static bool same_subnet(const uint8_t *left, const uint8_t *right, const uint8_t *mask)
{
    uint32_t mask_u32 = ip_to_u32(mask);
    return (ip_to_u32(left) & mask_u32) == (ip_to_u32(right) & mask_u32);
}

static bool nonzero_ip_is_valid(const uint8_t *ip4)
{
    return !is_zero_ip(ip4) && ip4[0] != 255u && ip4[3] != 255u;
}

static bool is_safe_hostname(const String &text)
{
    size_t len = text.length();
    if (len == 0 || len > sizeof(g_config.device_hostname) - 1) return false;
    if (text[0] == '-' || text[len - 1] == '-') return false;

    for (size_t i = 0; i < len; ++i) {
        char ch = text[i];
        if (!isalnum((unsigned char)ch) && ch != '-') return false;
    }
    return true;
}

static bool is_safe_label_text(const String &text, size_t max_len, bool allow_space)
{
    size_t len = text.length();
    if (len == 0 || len > max_len) return false;

    for (size_t i = 0; i < len; ++i) {
        unsigned char ch = (unsigned char)text[i];
        if (ch < 32u || ch > 126u) return false;
        if (!allow_space && ch == ' ') return false;
        if (ch == '\'' || ch == '"' || ch == '<' || ch == '>' || ch == '&') return false;
    }
    return true;
}

static bool is_safe_idn_name(const String &text)
{
    size_t len = text.length();
    if (len == 0 || len > sizeof(g_config.idn_response_name) - 1) return false;

    for (size_t i = 0; i < len; ++i) {
        char ch = text[i];
        if (!isalnum((unsigned char)ch) && ch != '-' && ch != '_' && ch != '.') return false;
    }
    return true;
}

static bool is_valid_ap_password_input(const String &text)
{
    size_t len = text.length();

    if (len == 0) {
#if FW_FORCE_RECOVERY_AP
        return true;
#else
        return false;
#endif
    }
    if (len < RECOVERY_AP_MIN_PASSWORD_LEN || len > sizeof(g_config.ap_password) - 1) {
        return false;
    }
    return is_safe_label_text(text, sizeof(g_config.ap_password) - 1, true);
}

static bool parse_u32(const String &text, uint32_t *value)
{
    char *end_ptr = NULL;
    unsigned long parsed;

    if (text.length() == 0) return false;
    parsed = strtoul(text.c_str(), &end_ptr, 10);
    if (end_ptr == NULL || *end_ptr != '\0') return false;
    *value = (uint32_t)parsed;
    return true;
}

static bool parse_u16(const String &text, uint16_t *value)
{
    uint32_t parsed = 0;
    if (!parse_u32(text, &parsed) || parsed > 65535u) return false;
    *value = (uint16_t)parsed;
    return true;
}

static uint8_t clamp_web_service_limit(uint32_t value)
{
    if (value < MIN_WEB_SERVICE_CLIENTS) {
        return MIN_WEB_SERVICE_CLIENTS;
    }
    if (value > MAX_WEB_SERVICE_CLIENTS) {
        return MAX_WEB_SERVICE_CLIENTS;
    }
    return (uint8_t)value;
}

static void append_html_escaped(String &out, const char *text)
{
    if (text == NULL || text[0] == '\0') {
        out += F("-");
        return;
    }

    while (*text) {
        switch (*text) {
        case '&': out += F("&amp;"); break;
        case '<': out += F("&lt;"); break;
        case '>': out += F("&gt;"); break;
        case '\'': out += F("&#39;"); break;
        case '"': out += F("&quot;"); break;
        default: out += *text; break;
        }
        ++text;
    }
}

static void append_ui_endpoints(String &page)
{
    bool has_endpoint = false;

    if (runtime_net_sta_connected()) {
        page += F("STA ");
        page += runtime_net_sta_ip().toString();
        has_endpoint = true;
    }
    if (runtime_net_lan_has_ip()) {
        if (has_endpoint) page += F(" | ");
        page += F("LAN ");
        page += runtime_net_lan_ip().toString();
        has_endpoint = true;
    }
    if (runtime_net_recovery_ap_active()) {
        if (has_endpoint) page += F(" | ");
        page += F("AP ");
        page += runtime_net_ap_ip().toString();
        has_endpoint = true;
    }
    if (!has_endpoint) {
        page += F("offline");
    }
}

static const char *reset_reason_text(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "poweron";
    case ESP_RST_EXT:       return "external";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "int_wdt";
    case ESP_RST_TASK_WDT:  return "task_wdt";
    case ESP_RST_WDT:       return "other_wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "sdio";
    default:                return "unknown";
    }
}

static void append_page_start(String &page, const char *title)
{
    page += F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
              "<meta name='viewport' content='width=device-width,initial-scale=1'>"
              "<title>");
    append_html_escaped(page, title);
    page += F("</title><style>"
              ":root{--bg:#ece8de;--card:#fffdf9;--ink:#14202b;--muted:#405261;--soft:#677784;--line:#cfc7b8;--accent:#0f766e;--accent-2:#d97706;--danger:#b42318;--banner-bg:#ebf7f4;--banner-ink:#134e4a;--banner-line:#a8d6ce;}"
              "body{margin:0;background:radial-gradient(circle at top,#fffdf6 0,#f1efe8 48%,#e8e2d6 100%);color:var(--ink);font-family:'Trebuchet MS','Lucida Sans Unicode','Segoe UI',sans-serif;font-size:16px;line-height:1.5;}"
              ".wrap{max-width:1180px;margin:0 auto;padding:20px 18px 36px;}"
              ".hero{background:linear-gradient(135deg,#113c38,#0f766e 58%,#e6b85c 160%);color:#f9f6ef;border-radius:18px;padding:20px 22px;box-shadow:0 18px 40px rgba(15,118,110,.18);}"
              ".hero h1{margin:0;font-size:1.75rem;letter-spacing:.03em;}"
              ".hero p{margin:.5rem 0 0;color:#eef8f5;font-size:1.02rem;}"
              ".banner{margin-top:14px;background:var(--banner-bg);color:var(--banner-ink);border:1px solid var(--banner-line);border-radius:12px;padding:12px 14px;font-weight:600;}"
              "nav{display:flex;flex-wrap:wrap;gap:8px;margin:18px 0 20px;}"
              "nav a{color:var(--ink);text-decoration:none;background:rgba(255,255,255,.78);border:1px solid var(--line);padding:8px 12px;border-radius:999px;font-size:.96rem;font-weight:600;}"
              "nav a.active{background:var(--accent);border-color:var(--accent);color:#fff;}"
              ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(240px,1fr));gap:16px;align-items:start;}"
              ".summary-grid{grid-template-columns:repeat(auto-fit,minmax(320px,1fr));}"
              ".card{background:var(--card);border:1px solid var(--line);border-radius:16px;padding:18px 20px;box-shadow:0 8px 22px rgba(23,32,42,.06);min-width:0;}"
              ".card h2,.card h3{margin:0 0 10px;font-size:1.08rem;line-height:1.35;}"
              ".muted{color:var(--soft);}"
              "form{display:grid;gap:14px;}"
              ".field{display:grid;gap:7px;min-width:0;}"
              ".field label{font-size:.82rem;color:var(--muted);font-weight:700;text-transform:uppercase;letter-spacing:.06em;}"
              "input[type=text],input[type=password],select{width:100%;box-sizing:border-box;padding:11px 12px;border:1px solid var(--line);border-radius:12px;background:#fff;color:var(--ink);font-size:1rem;min-width:0;}"
              "input::placeholder{color:var(--soft);}"
              "input[type=checkbox]{transform:translateY(1px);margin-right:8px;}"
              ".toggle{display:flex;align-items:flex-start;gap:8px;font-size:.96rem;font-weight:600;color:var(--ink);text-transform:none;letter-spacing:0;}"
              ".toggle input[type=checkbox]{flex:0 0 auto;transform:translateY(3px);margin:0;}"
              ".actions{display:flex;flex-wrap:wrap;gap:12px;margin-top:10px;}"
              ".btn,.btn-link{display:inline-block;text-decoration:none;border:none;border-radius:999px;padding:11px 16px;font-weight:700;font-size:.95rem;line-height:1.2;cursor:pointer;}"
              ".btn{background:var(--accent);color:#fff;}"
              ".btn.alt{background:#f6f0e4;color:var(--ink);border:1px solid var(--line);}"
              ".btn.warn{background:var(--accent-2);color:#fff;}"
              ".btn.danger{background:var(--danger);color:#fff;}"
              ".kv{display:grid;grid-template-columns:minmax(138px,220px) minmax(0,1fr);gap:10px 14px;align-items:start;}"
              ".kv b{font-weight:700;color:var(--muted);}"
              ".kv span{min-width:0;overflow-wrap:anywhere;word-break:break-word;}"
              "pre{margin:0;white-space:pre-wrap;overflow:auto;overflow-wrap:anywhere;background:#f7f4ec;border:1px solid var(--line);border-radius:12px;padding:12px;}"
              "table{width:100%;border-collapse:collapse;table-layout:fixed;}"
              "td{padding:8px 0;vertical-align:top;border-bottom:1px solid #eee8db;overflow-wrap:anywhere;word-break:break-word;}"
              "td:first-child{color:var(--muted);padding-right:16px;width:34%;font-weight:600;}"
              "small{color:var(--soft);display:block;line-height:1.45;}"
              "@media (max-width:760px){.wrap{padding:18px 14px 28px;}.summary-grid{grid-template-columns:1fr;}.kv{grid-template-columns:1fr;}td:first-child{width:auto;padding-right:0;}}"
              "</style><script>"
              "function togglePasswordField(fieldId,toggle){var field=document.getElementById(fieldId);if(field){field.type=toggle.checked?'text':'password';}}"
              "function setSectionVisible(sectionId,visible,displayMode){var section=document.getElementById(sectionId);if(!section)return;section.style.display=visible?(displayMode||'block'):'none';var fields=section.querySelectorAll('input,select,textarea');for(var i=0;i<fields.length;i++){fields[i].disabled=!visible;}}"
              "function syncWifiStaticFields(){var dhcp=document.querySelector(\"input[name='dhcp'][value='1']\");setSectionVisible('wifi_static_fields',!(dhcp&&dhcp.checked),'grid');}"
              "function scheduleAutoRefresh(ms){window.setTimeout(function(){window.location.reload();},ms);}"
              "document.addEventListener('DOMContentLoaded',function(){var radios=document.querySelectorAll(\"input[name='dhcp']\");for(var i=0;i<radios.length;i++){radios[i].addEventListener('change',syncWifiStaticFields);}syncWifiStaticFields();});"
              "</script></head><body><div class='wrap'><section class='hero'><h1>WT32 Bode Bridge</h1><p>");
    append_html_escaped(page, title);
    page += F("</p></section>");

    if (runtime_net_recovery_ap_active()) {
#if FW_KEEP_RECOVERY_AP_ALWAYS_ON
        page += F("<div class='banner'>Recovery AP is always active at 192.168.4.1. The UI remains available in parallel on AP, WiFi STA, and LAN.</div>");
#else
        page += F("<div class='banner'>Recovery AP is active. Configure WiFi STA and then use Save + Reboot.</div>");
#endif
    }
}

static void append_nav(String &page, const char *active)
{
    static const struct {
        const char *href;
        const char *label;
    } links[] = {
        { "/", "Status" },
        { "/network", "Network" },
        { "/scope", "Scope" },
        { "/fy6900", "FY6900" },
        { "/bode", "Bode" },
        { "/diag", "Diag" }
    };

    page += F("<nav>");
    for (size_t i = 0; i < sizeof(links) / sizeof(links[0]); ++i) {
        page += F("<a href='");
        page += links[i].href;
        page += F("'");
        if (active != NULL && strcmp(active, links[i].href) == 0) {
            page += F(" class='active'");
        }
        page += F(">");
        page += links[i].label;
        page += F("</a>");
    }
    page += F("</nav>");
}

static void append_page_end(String &page)
{
    page += F("</div></body></html>");
}

static const char *runtime_fail_reason_label(const char *reason)
{
    if (reason == NULL || reason[0] == '\0' || strcmp(reason, "none") == 0) {
        return "none";
    }
    if (strcmp(reason, "sta_connecting") == 0) {
        return "STA connecting";
    }
    if (strcmp(reason, "sta_bootstrap_ap") == 0) {
        return "Recovery AP active during STA bootstrap";
    }
    if (strcmp(reason, "forced_recovery_ap") == 0) {
        return "Recovery AP forced by safe build";
    }
    if (strcmp(reason, "permanent_ap") == 0) {
        return "AP active permanently";
    }
    if (strcmp(reason, "sta_boot_delay") == 0) {
        return "STA delayed after boot";
    }
    if (strcmp(reason, "no_sta_config") == 0) {
        return "No valid STA config";
    }
    if (strcmp(reason, "sta_config_failed") == 0) {
        return "STA static config failed";
    }
    if (strcmp(reason, "sta_connect_failed") == 0) {
        return "STA connect failed";
    }
    if (strcmp(reason, "sta_retry_exhausted") == 0) {
        return "STA retries exhausted";
    }
    if (strcmp(reason, "recovery_ap_disabled") == 0) {
        return "Recovery AP disabled";
    }
    if (strcmp(reason, "ap_config_failed") == 0) {
        return "Recovery AP config failed";
    }
    if (strcmp(reason, "ap_start_failed") == 0) {
        return "Recovery AP start failed";
    }
    if (strcmp(reason, "sta_lost_connection") == 0) {
        return "STA connection lost";
    }
    return reason;
}

static String runtime_status_detail(void)
{
    if (runtime_net_lan_has_ip() && runtime_net_sta_connected() && runtime_net_recovery_ap_active()) {
        return String(F("LAN active, WiFi STA connected, AP active"));
    }
    if (runtime_net_lan_has_ip() && runtime_net_sta_connected()) {
        return String(F("LAN active and WiFi STA connected"));
    }
    if (runtime_net_sta_connected() && runtime_net_recovery_ap_active()) {
        return String(F("WiFi STA connected while AP stays active"));
    }
    if (runtime_net_lan_has_ip() && runtime_net_recovery_ap_active()) {
        return String(F("LAN active with AP available"));
    }
    if (runtime_net_sta_connected()) {
        return String(F("WiFi STA connected"));
    }
    if (runtime_net_sta_connecting()) {
        if (runtime_net_recovery_ap_active()) {
            return String(F("WiFi STA connecting while Recovery AP keeps UI available"));
        }
        return String(F("WiFi STA connecting"));
    }
    if (runtime_net_sta_retry_exhausted()) {
        return String(F("Recovery AP active after finite STA retries were exhausted"));
    }
    if (runtime_net_recovery_ap_active()) {
#if FW_KEEP_RECOVERY_AP_ALWAYS_ON
        return String(F("AP active permanently and UI remains available"));
#endif
        if (strcmp(runtime_net_last_fail_reason(), "sta_lost_connection") == 0) {
            return String(F("Recovery AP active after STA connection was lost"));
        }
        if (strcmp(runtime_net_last_fail_reason(), "sta_connect_failed") == 0) {
            return String(F("Recovery AP active after STA connect failed"));
        }
        if (strcmp(runtime_net_last_fail_reason(), "no_sta_config") == 0) {
            return String(F("Recovery AP active because STA config is missing or invalid"));
        }
        return String(F("Recovery AP active"));
    }
    if (runtime_net_lan_has_ip()) {
        return String(F("LAN active"));
    }
    if (strcmp(runtime_net_last_fail_reason(), "recovery_ap_disabled") == 0) {
        if (runtime_net_sta_config_valid()) {
            return String(F("Offline because recovery AP is disabled after STA connect failed"));
        }
        return String(F("Offline because recovery AP is disabled and no valid STA config exists"));
    }
    if (strcmp(runtime_net_last_fail_reason(), "sta_connect_failed") == 0) {
        return String(F("Offline after STA connect failed"));
    }
    if (strcmp(runtime_net_last_fail_reason(), "no_sta_config") == 0) {
        return String(F("Offline because no valid STA config exists"));
    }
    return String(F("Offline"));
}

static const char *scope_proxy_status_text(void)
{
    const ScopeHttpProxyStats *stats = scope_http_proxy_get_stats();

    if (!stats->supported) {
        return "unsupported in this build";
    }
    if (!stats->enabled) {
        return "disabled";
    }
    if (!stats->listening) {
        return runtime_net_is_ready_for_web()
            ? "starting"
            : "waiting for web runtime";
    }
    if (stats->active_connections > 0u) {
        return "forwarding";
    }
    return "listening";
}

static const char *scope_proxy_hint_text(void)
{
    const ScopeHttpProxyStats *stats = scope_http_proxy_get_stats();

    if (stats->detected_absolute_location && stats->detected_absolute_scope_url) {
        return "absolute redirects and absolute URLs detected";
    }
    if (stats->detected_absolute_location) {
        return "absolute redirect detected";
    }
    if (stats->detected_absolute_scope_url) {
        return "absolute scope URL detected";
    }
    return "none seen";
}

static const char *scope_vnc_proxy_status_text(void)
{
    const ScopeVncProxyStats *stats = scope_vnc_proxy_get_stats();

    if (!stats->supported) {
        return "unsupported in this build";
    }
    if (!stats->enabled) {
        return "disabled";
    }
    if (!stats->listening) {
        return runtime_net_is_ready_for_web()
            ? "starting"
            : "waiting for web runtime";
    }
    if (stats->active_connections > 0u) {
        return "forwarding";
    }
    return "listening";
}

static const char *scope_vnc_proxy_hint_text(void)
{
    const ScopeVncProxyStats *stats = scope_vnc_proxy_get_stats();

    if (stats->detected_websocket_upgrade && stats->detected_websockify_path) {
        return "websocket upgrade and /websockify detected";
    }
    if (stats->detected_websocket_upgrade) {
        return "websocket upgrade detected";
    }
    if (stats->detected_websockify_path) {
        return "/websockify path detected";
    }
    return "none seen";
}

static void append_summary_cards(String &page)
{
    page += F("<section class='grid summary-grid'>"
              "<article class='card'><h2>Runtime</h2><div class='kv'>"
              "<b>Mode</b><span>");
    page += runtime_net_mode_text();
    page += F("</span><b>UI endpoints</b><span>");
    append_ui_endpoints(page);
    page += F("</span><b>Hostname</b><span>");
    page += g_config.device_hostname;
    page += F("</span><b>Status</b><span>");
    page += runtime_status_detail();
    page += F("</span><b>Web UI</b><span>");
    page += runtime_net_is_ready_for_web() ? F("ready") : F("offline");
    page += F("</span><b>Bode services</b><span>");
    if (net_is_running()) {
        page += F("running");
    } else if (runtime_net_should_run_bode()) {
        page += strcmp(fy_status_text(), "available") == 0
            ? F("ready")
            : F("waiting for FY");
    } else {
        page += F("stopped");
    }
    page += F("</span><b>Time</b><span>");
    page += runtime_net_time_status_text();
    page += F("</span></div></article>");

    page += F("<article class='card'><h2>LAN / NTP</h2><div class='kv'><b>LAN link</b><span>");
    page += runtime_net_lan_link_up() ? F("up") : F("down");
    page += F("</span><b>LAN IP</b><span>");
    page += runtime_net_lan_ip().toString();
    page += F("</span><b>LAN mask</b><span>");
    page += runtime_net_lan_subnet().toString();
    page += F("</span><b>LAN gateway</b><span>");
    page += eth_gateway_ip().toString();
    page += F("</span><b>LAN DNS</b><span>");
    page += eth_dns_ip().toString();
    page += F("</span><b>NTP server</b><span>");
    page += runtime_net_ntp_server_running() ? F("on") : F("off");
    page += F("</span><b>NTP served</b><span>");
    page += String((unsigned long)runtime_net_ntp_request_count());
    page += F("</span><b>Time source</b><span>");
    page += runtime_net_time_status_text();
    page += F("</span><b>Scope IP</b><span>");
    page += ip_to_str(g_config.scope_ip);
    page += F(":");
    page += String((unsigned)g_config.scope_port);
    page += F("</span><b>HTTP proxy</b><span>");
    page += scope_proxy_status_text();
    page += F("</span><b>Proxy port</b><span>");
    page += String((unsigned)scope_http_proxy_listen_port());
    page += F("</span><b>noVNC proxy</b><span>");
    page += scope_vnc_proxy_status_text();
    page += F("</span><b>noVNC clients</b><span>");
    page += String((unsigned)scope_vnc_proxy_get_stats()->active_connections);
    page += F("/");
    page += String((unsigned)scope_vnc_proxy_get_stats()->max_connections);
    page += F("</span><b>noVNC port</b><span>");
    page += String((unsigned)scope_vnc_proxy_listen_port());
    page += F("</span></div></article>");

    page += F("<article class='card'><h2>WiFi / AP</h2><div class='kv'><b>STA config</b><span>");
    page += runtime_net_sta_config_valid() ? F("valid") : F("missing or invalid");
    page += F("</span><b>STA mode</b><span>");
    page += g_config.use_dhcp ? F("DHCP") : F("static");
    page += F("</span><b>STA state</b><span>");
    if (runtime_net_sta_connected()) {
        page += F("connected");
    } else if (runtime_net_sta_connecting()) {
        page += F("connecting");
    } else if (runtime_net_sta_retry_exhausted()) {
        page += F("disconnected (retries exhausted)");
    } else {
        page += F("disconnected");
    }
    page += F("</span><b>SSID</b><span>");
    append_html_escaped(page, runtime_net_ssid()[0] ? runtime_net_ssid() : g_config.wifi_ssid);
    page += F("</span><b>STA IP</b><span>");
    page += runtime_net_sta_ip().toString();
    page += F("</span><b>STA retries</b><span>");
    page += String((unsigned)runtime_net_sta_retry_count());
    page += F("/");
    page += String((unsigned)runtime_net_sta_retry_limit());
    page += F("</span><b>AP policy</b><span>");
#if FW_KEEP_RECOVERY_AP_ALWAYS_ON
    page += F("permanent in this build");
#else
    page += g_config.recovery_ap_enable ? F("fallback enabled") : F("fallback disabled");
#endif
    page += F("</span><b>AP state</b><span>");
    if (runtime_net_recovery_ap_active()) {
#if FW_KEEP_RECOVERY_AP_ALWAYS_ON
        page += F("active (permanent)");
#else
        page += F("active");
#endif
    } else if (FW_KEEP_RECOVERY_AP_ALWAYS_ON) {
        page += F("expected active, check fail reason");
    } else if (runtime_net_mode() == RUNTIME_NET_NONE && !g_config.recovery_ap_enable) {
        page += F("disabled");
    } else {
        page += F("standby");
    }
    page += F("</span><b>AP SSID</b><span>");
    append_html_escaped(page, runtime_net_recovery_ap_active() ? runtime_net_ap_ssid() : g_config.ap_ssid);
    page += F("</span><b>AP security</b><span>");
#if FW_FORCE_RECOVERY_AP
    page += F("open (forced safe build)");
#else
    page += g_config.ap_password[0] ? F("secured") : F("open");
#endif
    page += F("</span><b>AP IP</b><span>");
    page += runtime_net_ap_ip().toString();
    page += F("</span><b>Last fail</b><span>");
    page += runtime_fail_reason_label(runtime_net_last_fail_reason());
    page += F("</span></div></article>");

    page += F("<article class='card'><h2>Scope / FY6900</h2><div class='kv'><b>Scope probe</b><span>");
    if (!runtime_net_scope_probe_available()) {
        page += F("not active");
    } else {
        page += runtime_net_scope_reachable() ? F("reachable") : F("unreachable");
    }
    page += F("</span><b>Scope IP</b><span>");
    page += ip_to_str(g_config.scope_ip);
    page += F(":");
    page += String((unsigned)g_config.scope_port);
    page += F("</span><b>FY baud</b><span>");
    page += String((unsigned long)fy_get_baud());
    page += F("</span><b>FY serial</b><span>");
    page += fy_get_serial_mode();
    page += F("</span><b>FY status</b><span>");
    page += fy_status_text();
    page += F("</span><b>FY enabled</b><span>");
    page += fy_is_enabled() ? F("yes") : F("no");
    page += F("</span><b>Proxy clients</b><span>");
    page += String((unsigned)scope_http_proxy_get_stats()->active_connections);
    page += F("</span><b>noVNC clients</b><span>");
    page += String((unsigned)scope_vnc_proxy_get_stats()->active_connections);
    page += F("</span></div></article></section>");
}

static void append_fy_diag_section(String &page)
{
#if FW_ENABLE_FY_SERIAL_DIAG
    FyDiagSnapshot diag;
    fy_diag_get_snapshot(&diag);

    page += F("<section class='card'><h2>FY Serial Diagnostics</h2><table>"
              "<tr><td>Serial2 initialized</td><td>");
    page += diag.serial_initialized ? F("yes") : F("no");
    page += F("</td></tr><tr><td>FY present</td><td>");
    page += fy_diag_presence_text();
    page += F("</td></tr><tr><td>Status</td><td>");
    append_html_escaped(page, diag.current_status);
    page += F("</td></tr><tr><td>Serial format</td><td>");
    append_html_escaped(page, diag.serial_mode);
    page += F("</td></tr><tr><td>Baud</td><td>");
    page += String((unsigned long)diag.baud);
    page += F("</td></tr><tr><td>Pins</td><td>RX=");
    page += String((unsigned)diag.rx_pin);
    page += F(" TX=");
    page += String((unsigned)diag.tx_pin);
    page += F("</td></tr><tr><td>Timeout</td><td>");
    page += String((unsigned)diag.timeout_ms);
    page += F(" ms</td></tr><tr><td>TX total</td><td>");
    page += String((unsigned long)diag.tx_total_bytes);
    page += F("</td></tr><tr><td>RX total</td><td>");
    page += String((unsigned long)diag.rx_total_bytes);
    page += F("</td></tr><tr><td>Timeout count</td><td>");
    page += String((unsigned long)diag.timeout_count);
    page += F("</td></tr><tr><td>Invalid RX count</td><td>");
    page += String((unsigned long)diag.invalid_response_count);
    page += F("</td></tr><tr><td>Last operation</td><td>");
    append_html_escaped(page, diag.last_operation);
    page += F(" #");
    page += String((unsigned long)diag.operation_count);
    page += F(" @ ");
    page += String((unsigned long)diag.last_operation_ms);
    page += F(" ms</td></tr><tr><td>Last error</td><td>");
    append_html_escaped(page, diag.last_error[0] ? diag.last_error : "none");
    page += F("</td></tr><tr><td>Last response kind</td><td>");
    append_html_escaped(page, diag.last_response_kind[0] ? diag.last_response_kind : "unknown");
    page += F("</td></tr><tr><td>Last TX raw</td><td><pre>");
    append_html_escaped(page, diag.last_tx_raw[0] ? diag.last_tx_raw : "(empty)");
    page += F("</pre></td></tr><tr><td>Last TX HEX</td><td><pre>");
    append_html_escaped(page, diag.last_tx_hex[0] ? diag.last_tx_hex : "(empty)");
    page += F("</pre></td></tr><tr><td>Last RX raw</td><td><pre>");
    append_html_escaped(page, diag.last_rx_raw[0] ? diag.last_rx_raw : "(empty)");
    page += F("</pre></td></tr><tr><td>Last RX ASCII</td><td><pre>");
    append_html_escaped(page, diag.last_rx_ascii[0] ? diag.last_rx_ascii : "(empty)");
    page += F("</pre></td></tr><tr><td>Last RX HEX</td><td><pre>");
    append_html_escaped(page, diag.last_rx_hex[0] ? diag.last_rx_hex : "(empty)");
    page += F("</pre></td></tr></table>");

    page += F("<div class='actions'>"
              "<a class='btn warn' href='/fy-diag-summary'>Diag Summary</a>"
              "<a class='btn alt' href='/fy-diag-init'>Init Serial2</a>"
              "<a class='btn alt' href='/fy-diag-reinit'>Reinit UART</a>"
              "<a class='btn alt' href='/fy-diag-deinit'>Deinit Serial2</a>"
              "<a class='btn alt' href='/fy-diag-flush'>Flush RX</a>"
              "<a class='btn warn' href='/fy-diag-probe?timeout_ms=");
    page += String((unsigned)g_config.awg_serial_timeout_ms);
    page += F("'>Probe UMO</a>"
              "<a class='btn alt' href='/fy-diag-compare?timeout_ms=");
    page += String((unsigned)g_config.awg_serial_timeout_ms);
    page += F("'>Compare 8N2/8N1</a>"
              "<a class='btn alt' href='/fy-diag-read?timeout_ms=");
    page += String((unsigned)g_config.awg_serial_timeout_ms);
    page += F("'>Read Raw</a></div>");

    page += F("<form method='post' action='/fy-diag-send'>"
              "<div class='grid'><div class='field'><label>Raw Command</label>"
              "<input type='text' name='raw_cmd' value='UMO'></div>"
              "<div class='field'><label>Read Timeout (ms)</label>"
              "<input type='text' name='timeout_ms' value='");
    page += String((unsigned)g_config.awg_serial_timeout_ms);
    page += F("'></div><div class='field'><label>Options</label>"
              "<label><input type='checkbox' name='append_newline' value='1' checked>Append newline</label></div></div>"
              "<div class='actions'><button class='btn' type='submit'>Send Raw Command</button></div></form></section>");
#endif
}

static void append_fy_diag_snapshot_html(String &page)
{
#if FW_ENABLE_FY_SERIAL_DIAG
    FyDiagSnapshot diag;
    fy_diag_get_snapshot(&diag);

    page += F("Serial2 initialized: ");
    page += diag.serial_initialized ? F("yes") : F("no");
    page += F("<br>FY present: ");
    page += fy_diag_presence_text();
    page += F("<br>Status: ");
    append_html_escaped(page, diag.current_status);
    page += F("<br>Serial format: ");
    append_html_escaped(page, diag.serial_mode);
    page += F("<br>Baud: ");
    page += String((unsigned long)diag.baud);
    page += F("<br>Pins: RX=");
    page += String((unsigned)diag.rx_pin);
    page += F(" TX=");
    page += String((unsigned)diag.tx_pin);
    page += F("<br>Timeout: ");
    page += String((unsigned)diag.timeout_ms);
    page += F(" ms<br>TX total: ");
    page += String((unsigned long)diag.tx_total_bytes);
    page += F("<br>RX total: ");
    page += String((unsigned long)diag.rx_total_bytes);
    page += F("<br>Timeout count: ");
    page += String((unsigned long)diag.timeout_count);
    page += F("<br>Invalid RX count: ");
    page += String((unsigned long)diag.invalid_response_count);
    page += F("<br>Last operation: ");
    append_html_escaped(page, diag.last_operation);
    page += F(" #");
    page += String((unsigned long)diag.operation_count);
    page += F(" @ ");
    page += String((unsigned long)diag.last_operation_ms);
    page += F(" ms<br>Last error: ");
    append_html_escaped(page, diag.last_error[0] ? diag.last_error : "none");
    page += F("<br>Last response kind: ");
    append_html_escaped(page, diag.last_response_kind[0] ? diag.last_response_kind : "unknown");
    page += F("<br><br>Last TX raw:<pre>");
    append_html_escaped(page, diag.last_tx_raw[0] ? diag.last_tx_raw : "(empty)");
    page += F("</pre>Last TX HEX:<pre>");
    append_html_escaped(page, diag.last_tx_hex[0] ? diag.last_tx_hex : "(empty)");
    page += F("</pre>Last RX raw:<pre>");
    append_html_escaped(page, diag.last_rx_raw[0] ? diag.last_rx_raw : "(empty)");
    page += F("</pre>Last RX ASCII:<pre>");
    append_html_escaped(page, diag.last_rx_ascii[0] ? diag.last_rx_ascii : "(empty)");
    page += F("</pre>Last RX HEX:<pre>");
    append_html_escaped(page, diag.last_rx_hex[0] ? diag.last_rx_hex : "(empty)");
    page += F("</pre>Last UMO raw:<pre>");
    append_html_escaped(page, diag.last_umo_raw[0] ? diag.last_umo_raw : "(empty)");
    page += F("</pre>Last UMO parsed:<pre>");
    append_html_escaped(page, diag.last_umo_parsed[0] ? diag.last_umo_parsed : "(empty)");
    page += F("</pre>UMO parser status:<pre>");
    append_html_escaped(page, diag.last_umo_parser_status[0] ? diag.last_umo_parser_status : "not requested");
    page += F("</pre>Last parser readback:<pre>");
    append_html_escaped(page, diag.last_parser_readback[0] ? diag.last_parser_readback : "(empty)");
    page += F("</pre>Last sequence result:<pre>");
    append_html_escaped(page, diag.last_sequence_result[0] ? diag.last_sequence_result : "not requested");
    page += F("</pre>Functional level:<pre>");
    page += String((unsigned)diag.last_functional_level);
    page += F("</pre>Functional success:<pre>");
    page += diag.last_functional_success ? F("yes") : F("no");
    page += F("</pre>");
#endif
}

static void append_fy_final_test_section(String &page)
{
#if FW_ENABLE_FY_FINAL_TEST
    FyDiagSnapshot diag;
    fy_diag_get_snapshot(&diag);

    page += F("<section class='card'><h2>FY Final Test Safe</h2><table>"
              "<tr><td>Mode</td><td>manual only, safe-first</td></tr>"
              "<tr><td>FY detected</td><td>");
    page += strcmp(fy_diag_presence_text(), "present") == 0 ? F("yes") : F("no");
    page += F("</td></tr><tr><td>Last UMO raw</td><td><pre>");
    append_html_escaped(page, diag.last_umo_raw[0] ? diag.last_umo_raw : "(empty)");
    page += F("</pre></td></tr><tr><td>Last UMO parsed</td><td><pre>");
    append_html_escaped(page, diag.last_umo_parsed[0] ? diag.last_umo_parsed : "(empty)");
    page += F("</pre></td></tr><tr><td>Parser status</td><td>");
    append_html_escaped(page, diag.last_umo_parser_status[0] ? diag.last_umo_parser_status : "not requested");
    page += F("</td></tr><tr><td>Last sequence result</td><td><pre>");
    append_html_escaped(page, diag.last_sequence_result[0] ? diag.last_sequence_result : "not requested");
    page += F("</pre></td></tr><tr><td>Last parser readback</td><td><pre>");
    append_html_escaped(page, diag.last_parser_readback[0] ? diag.last_parser_readback : "(empty)");
    page += F("</pre></td></tr><tr><td>Functional level</td><td>");
    page += String((unsigned)diag.last_functional_level);
    page += F("</td></tr><tr><td>Functional success</td><td>");
    page += diag.last_functional_success ? F("yes") : F("no");
    page += F("</td></tr></table><p class='muted'>This path keeps Recovery AP and Web UI up, then validates UMO parsing plus a minimal SCPI write/query sequence with outputs kept OFF.</p>"
              "<div class='actions'><a class='btn warn' href='/fy-final-umo?run=yes'>Re-test UMO</a>"
              "<a class='btn' href='/fy-final-test?run=yes'>Run Minimal Functional FY Test</a>"
              "<a class='btn alt' href='/fy-final-reset'>Reset FY Software State</a></div></section>");
#endif
}

static void send_page(const String &page)
{
    s_server.send(200, "text/html", page);
}

static void send_message_page(const char *title, const String &message,
    bool is_error, const char *back_href, const char *back_label, bool show_reboot)
{
    String page;
    page.reserve(2200);
    append_page_start(page, title);
    append_nav(page, NULL);
    page += F("<section class='card'><h2>");
    append_html_escaped(page, title);
    page += F("</h2><p");
    if (is_error) page += F(" style='color:var(--danger)' ");
    page += F(">");
    page += message;
    page += F("</p><div class='actions'>");
    if (back_href != NULL && back_label != NULL) {
        page += F("<a class='btn alt' href='");
        page += back_href;
        page += F("'>");
        page += back_label;
        page += F("</a>");
    }
    if (show_reboot) {
        page += F("<a class='btn warn' href='/reboot?confirm=yes'>Reboot now</a>");
    }
    page += F("</div></section>");
    append_page_end(page);
    send_page(page);
}

static bool ip_changed(const uint8_t *left, const uint8_t *right)
{
    return memcmp(left, right, 4u) != 0;
}

static bool network_settings_changed(const EspConfig &left, const EspConfig &right)
{
    return left.use_dhcp != right.use_dhcp
        || left.recovery_ap_enable != right.recovery_ap_enable
        || strcmp(left.device_hostname, right.device_hostname) != 0
        || strcmp(left.ntp_server, right.ntp_server) != 0
        || strcmp(left.wifi_ssid, right.wifi_ssid) != 0
        || strcmp(left.wifi_password, right.wifi_password) != 0
        || strcmp(left.ap_ssid, right.ap_ssid) != 0
        || strcmp(left.ap_password, right.ap_password) != 0
        || ip_changed(left.ip, right.ip)
        || ip_changed(left.mask, right.mask)
        || ip_changed(left.gw, right.gw)
        || ip_changed(left.dns, right.dns)
        || ip_changed(left.dns2, right.dns2)
        || ip_changed(left.lan_ip, right.lan_ip)
        || ip_changed(left.lan_mask, right.lan_mask);
}

static bool fy_settings_changed(const EspConfig &left, const EspConfig &right)
{
    return left.awg_baud != right.awg_baud
        || left.awg_serial_timeout_ms != right.awg_serial_timeout_ms
        || strcmp(left.awg_serial_mode, right.awg_serial_mode) != 0;
}

static const char *save_back_path(const String &section)
{
    if (section == "network" || section == "lan" || section == "service_limits") {
        return "/network";
    }
    if (section == "scope" || section == "scope_proxy") {
        return "/scope";
    }
    if (section == "fy6900") {
        return "/fy6900";
    }
    return "/bode";
}

static void append_protocol_diag_section(String &page)
{
    const NetStats *stats = net_get_stats();
    const ScopeHttpProxyStats *http_stats = scope_http_proxy_get_stats();
    const ScopeVncProxyStats *vnc_stats = scope_vnc_proxy_get_stats();
    char buf[192];

    page += F("<section class='card'><h2>Protocol Diagnostics</h2>");
    if (!net_is_running()) {
        page += F("<p class='muted'>Bode/VXI services are not running in the current mode.</p></section>");
        return;
    }

    page += F("<p class='muted'>This section tracks RPC/VXI session state. HTTP proxy and noVNC clients are separate runtime counters.</p><pre>");
    snprintf(buf, sizeof(buf),
        "snapshot_ms=%lu\ncurrent_vxi_port=%u\nvxi_session_active=%s\nlast_event=%s\n"
        "http_proxy_active_clients=%u\nnovnc_proxy_active_clients=%u\n"
        "udp_getport_count=%lu\ntcp_getport_count=%lu\ngetport_zero_reply_count=%lu\n"
        "create_link_count=%lu\ndevice_write_count=%lu\ndevice_read_count=%lu\n"
        "destroy_link_count=%lu\nsession_accept_count=%lu\nsession_end_count=%lu\n"
        "session_drop_count=%lu\nunknown_proc_count=%lu\nmalformed_packet_count=%lu\n",
        (unsigned long)millis(),
        stats->current_vxi_port,
        stats->session_active ? "yes" : "no",
        stats->last_event,
        (unsigned)http_stats->active_connections,
        (unsigned)vnc_stats->active_connections,
        (unsigned long)stats->udp_getport_count,
        (unsigned long)stats->tcp_getport_count,
        (unsigned long)stats->getport_zero_reply_count,
        (unsigned long)stats->create_link_count,
        (unsigned long)stats->device_write_count,
        (unsigned long)stats->device_read_count,
        (unsigned long)stats->destroy_link_count,
        (unsigned long)stats->session_accept_count,
        (unsigned long)stats->session_end_count,
        (unsigned long)stats->session_drop_count,
        (unsigned long)stats->unknown_proc_count,
        (unsigned long)stats->malformed_packet_count);
    page += buf;
    snprintf(buf, sizeof(buf),
        "last_write_len=%lu\nlast_read_len=%lu\nlast_read_declared_len=%lu\n"
        "active_vxi_client=%s:%u\nreset_reason=%s\n",
        (unsigned long)stats->last_write_len,
        (unsigned long)stats->last_read_len,
        (unsigned long)stats->last_read_declared_len,
        stats->active_client_ip.toString().c_str(),
        stats->active_client_port,
        reset_reason_text());
    page += buf;
    page += F("</pre></section>");
}

static void handle_root(void)
{
    String page;
    page.reserve(4500);
    append_page_start(page, "Status");
    append_nav(page, "/");
    append_summary_cards(page);

    page += F("<section class='grid summary-grid'>"
              "<article class='card'><h2>Open URLs</h2><table>"
              "<tr><td>Normal runtime</td><td>");
    if (runtime_net_sta_connected()) {
        page += F("http://");
        page += runtime_net_sta_ip().toString();
        page += F("/");
    } else if (runtime_net_sta_connecting()) {
        page += F("pending while STA connects");
    } else if (runtime_net_recovery_ap_active()) {
        page += F("not active while recovery AP is running");
    } else {
        page += F("disabled while device is offline");
    }
    page += F("</td></tr><tr><td>LAN</td><td>");
    if (runtime_net_lan_has_ip()) {
        page += F("http://");
        page += runtime_net_lan_ip().toString();
        page += F("/");
    } else {
        page += F("LAN not ready");
    }
    page += F("</td></tr><tr><td>AP</td><td>");
    if (runtime_net_recovery_ap_active()) {
        page += F("http://192.168.4.1/");
    } else {
#if FW_KEEP_RECOVERY_AP_ALWAYS_ON
        page += F("expected active, see fail reason");
#else
        if (g_config.recovery_ap_enable) {
            page += F("available only on STA failure");
        } else {
            page += F("disabled in config");
        }
#endif
    }
    page += F("</td></tr><tr><td>Scope proxy</td><td>");
    if (scope_http_proxy_is_listening()) {
        bool have_url = false;

        if (runtime_net_sta_connected()) {
            page += F("http://");
            page += runtime_net_sta_ip().toString();
            page += F(":");
            page += String((unsigned)scope_http_proxy_listen_port());
            page += F("/");
            have_url = true;
        }
        if (runtime_net_recovery_ap_active()) {
            if (have_url) {
                page += F(" | ");
            }
            page += F("http://");
            page += runtime_net_ap_ip().toString();
            page += F(":");
            page += String((unsigned)scope_http_proxy_listen_port());
            page += F("/");
            have_url = true;
        }
        if (!have_url) {
            page += F("listening, waiting for client-facing IP");
        }
    } else {
        page += scope_proxy_status_text();
    }
    page += F("</td></tr><tr><td>noVNC WebSocket</td><td>");
    if (scope_vnc_proxy_is_listening()) {
        bool have_url = false;

        if (runtime_net_sta_connected()) {
            page += F("ws://");
            page += runtime_net_sta_ip().toString();
            page += F(":");
            page += String((unsigned)scope_vnc_proxy_listen_port());
            page += F("/websockify");
            have_url = true;
        }
        if (!have_url && runtime_net_recovery_ap_active()) {
            page += F("ws://");
            page += runtime_net_ap_ip().toString();
            page += F(":");
            page += String((unsigned)scope_vnc_proxy_listen_port());
            page += F("/websockify");
            have_url = true;
        }
        if (!have_url) {
            page += F("listening, waiting for client-facing IP");
        }
    } else {
        page += scope_vnc_proxy_status_text();
    }
    page += F("</td></tr><tr><td>Offline reason</td><td>");
    page += runtime_status_detail();
    page += F("</td></tr><tr><td>Build variant</td><td>");
    page += FW_VARIANT_NAME;
    page += F("</td></tr></table></article>");

    page += F("<article class='card'><h2>Links</h2>"
              "<div class='actions'>"
              "<a class='btn alt' href='/network'>Configure Network</a>"
              "<a class='btn alt' href='/scope'>Configure Scope</a>"
              "<a class='btn alt' href='/fy6900'>Configure FY6900</a>"
              "<a class='btn alt' href='/bode'>Configure Bode</a>"
              "<a class='btn alt' href='/diag'>Diagnostics</a>"
              "<a class='btn warn' href='/reboot'>Reboot</a>"
              "<a class='btn danger' href='/factory-reset'>Factory Reset</a>"
              "</div></article></section>");

    append_page_end(page);
    send_page(page);
}

static void handle_network_get(void)
{
    const WebUiServerStats *web_stats = webconfig_get_stats();
    String page;
    page.reserve(13200);
    append_page_start(page, "Network");
    append_nav(page, "/network");
    append_summary_cards(page);

    page += F("<section class='card'><h2>WiFi STA / AP</h2><form method='post' action='/save'>"
              "<input type='hidden' name='section' value='network'>"
              "<div class='field'><label>Hostname</label><input type='text' name='device_hostname' maxlength='32' value='");
    page += g_config.device_hostname;
    page += F("'></div><div class='field'><label>Primary NTP Server</label><input type='text' name='ntp_server' maxlength='32' value='");
    page += g_config.ntp_server;
    page += F("'></div><div class='field'><label>WiFi SSID</label><input type='text' name='wifi_ssid' maxlength='32' value='");
    page += g_config.wifi_ssid;
    page += F("'></div><div class='field'><label>WiFi Password</label><input type='password' id='wifi_password' name='wifi_password' maxlength='63' value='");
    page += g_config.wifi_password;
    page += F("'><label class='toggle'><input type='checkbox' id='wifi_password_toggle' onclick=\"togglePasswordField('wifi_password', this)\">Show WiFi STA password</label></div><div class='field'><label>Addressing</label><div><label><input type='radio' name='dhcp' value='1'");
    if (g_config.use_dhcp) page += F(" checked");
    page += F("> DHCP</label> <label><input type='radio' name='dhcp' value='0'");
    if (!g_config.use_dhcp) page += F(" checked");
    page += F("> Static</label></div></div>");

    page += F("<div class='grid' id='wifi_static_fields'>"
              "<div class='field'><label>WiFi IP</label><input type='text' name='ip' value='");
    page += ip_to_str(g_config.ip);
    page += F("'></div><div class='field'><label>Subnet Mask</label><input type='text' name='mask' value='");
    page += ip_to_str(g_config.mask);
    page += F("'></div><div class='field'><label>Gateway</label><input type='text' name='gw' value='");
    page += ip_to_str(g_config.gw);
    page += F("'></div><div class='field'><label>DNS1</label><input type='text' name='dns' value='");
    page += ip_to_str(g_config.dns);
    page += F("'></div><div class='field'><label>DNS2</label><input type='text' name='dns2' value='");
    page += ip_to_str_or_empty(g_config.dns2);
    page += F("'></div></div><small>These WiFi STA fields apply only when Addressing is set to Static. When DHCP is selected, the stored static tuple is kept but not applied.</small>");

    page += F("<div class='grid'><div class='field'><label>AP SSID</label><input type='text' name='ap_ssid' maxlength='32' value='");
    page += g_config.ap_ssid;
    page += F("'></div><div class='field'><label>AP Password</label><input type='password' id='ap_password' name='ap_password' maxlength='63' value='");
    page += g_config.ap_password;
    page += F("'><label class='toggle'><input type='checkbox' id='ap_password_toggle' onclick=\"togglePasswordField('ap_password', this)\">Show AP password</label></div></div>");

    page += F("<div class='field'><label><input type='checkbox' name='recovery_ap_enable' value='1'");
    if (g_config.recovery_ap_enable) page += F(" checked");
    page += F("> Recovery AP enabled</label>");
#if FW_KEEP_RECOVERY_AP_ALWAYS_ON
    page += F("<small>In this final build, the AP stays active permanently and the UI remains reachable at the same time on AP, WiFi STA, and LAN whenever the Ethernet link is up.</small></div>");
#else
    page += F("<small>Leave the SSID blank if you want AP fallback plus LAN-only behavior. In the final safe build, the AP may start temporarily during boot or after a STA failure to keep the UI reachable.</small></div>");
#endif
#if FW_FORCE_RECOVERY_AP
    page += F("<small>This safe build forces an open recovery AP at boot. The SSID and password saved here remain stored, but they are used by the normal builds, not by this forced safe build.</small>");
#elif FW_KEEP_RECOVERY_AP_ALWAYS_ON
    page += F("<small>In the normal final build, the AP always uses the SSID and password saved here and stays active even after STA connects.</small>");
#else
    page += F("<small>In the normal final build, the recovery AP uses the SSID and password saved here.</small>");
#endif
    page += F("<div class='actions'><button class='btn' type='submit'>Save</button></div></form></section>");

    page += F("<section class='card'><h2>Local IP</h2><form method='post' action='/save'>"
              "<input type='hidden' name='section' value='lan'>"
              "<div class='grid'><div class='field'><label>Local IP</label><input type='text' name='lan_ip' value='");
    page += ip_to_str(g_config.lan_ip);
    page += F("'></div><div class='field'><label>Subnet Mask</label><input type='text' name='lan_mask' value='");
    page += ip_to_str(g_config.lan_mask);
    page += F("'></div></div><small>This dedicated Ethernet LAN stays static. Configure the oscilloscope target address in the Scope tab. Gateway and DNS remain 0.0.0.0 here, while the UI and the minimal NTP server stay available on this address whenever the Ethernet link is up.</small>"
              "<div class='actions'><button class='btn' type='submit'>Save LAN</button></div></form></section>");

    page += F("<section class='card'><h2>HTTP Service Limits</h2><form method='post' action='/save'>"
              "<input type='hidden' name='section' value='service_limits'>"
              "<div class='grid'>"
              "<div class='field'><label>Web UI max clients (:80)</label><input type='number' min='2' max='30' step='1' name='max_web_ui_clients' value='");
    page += String((unsigned)g_config.max_web_ui_clients);
    page += F("'><small>Allowed range: 2..30. Active now: ");
    page += String((unsigned)web_stats->active_clients);
    page += F(" / ");
    page += String((unsigned)web_stats->max_clients);
    page += F(".</small></div><div class='field'><label>Scope HTTP proxy max clients (:100)</label><input type='number' min='2' max='30' step='1' name='max_scope_http_proxy_clients' value='");
    page += String((unsigned)g_config.max_scope_http_proxy_clients);
    page += F("'><small>Allowed range: 2..30. Active now: ");
    page += String((unsigned)scope_http_proxy_get_stats()->active_connections);
    page += F(" / ");
    page += String((unsigned)scope_http_proxy_get_stats()->max_connections);
    page += F(".</small></div><div class='field'><label>noVNC / WebSocket max clients (:5900)</label><input type='number' min='2' max='30' step='1' name='max_scope_vnc_proxy_clients' value='");
    page += String((unsigned)g_config.max_scope_vnc_proxy_clients);
    page += F("'><small>Allowed range: 2..30. Active now: ");
    page += String((unsigned)scope_vnc_proxy_get_stats()->active_connections);
    page += F(" / ");
    page += String((unsigned)scope_vnc_proxy_get_stats()->max_connections);
    page += F(".</small></div></div>"
              "<small>Each limit is independent. When a limit is reached, the extra HTTP/WebSocket connection receives HTTP 503 and is closed immediately.</small>"
              "<div class='actions'><button class='btn' type='submit'>Save Limits</button></div></form></section>");

    page += F("<section class='card'><h2>NTP Policy</h2><table>"
              "<tr><td>NTP LAN only</td><td>");
    page += runtime_net_ntp_lan_only() ? F("yes") : F("no");
    page += F("</td></tr><tr><td>LAN subnet restriction active</td><td>");
    page += runtime_net_ntp_subnet_restriction_active() ? F("yes") : F("no");
    page += F("</td></tr><tr><td>NTP rate limit active</td><td>");
    page += runtime_net_ntp_rate_limit_active() ? F("yes") : F("no");
    page += F("</td></tr><tr><td>NTP rate limit</td><td>");
    page += String((unsigned)runtime_net_ntp_rate_limit_per_ip());
    page += F(" per IP / ");
    page += String((unsigned)runtime_net_ntp_rate_limit_global());
    page += F(" global per ");
    page += String((unsigned)NTP_RATE_LIMIT_WINDOW_MS);
    page += F(" ms</td></tr><tr><td>NTP policy drops</td><td>");
    page += String((unsigned long)runtime_net_ntp_policy_drop_count());
    page += F("</td></tr><tr><td>NTP rate-limit drops</td><td>");
    page += String((unsigned long)runtime_net_ntp_rate_limit_drop_count());
    page += F("</td></tr></table></section>");

    append_page_end(page);
    send_page(page);
}

static void handle_scope_get(void)
{
    const ScopeHttpProxyStats *proxy_stats = scope_http_proxy_get_stats();
    const ScopeVncProxyStats *vnc_stats = scope_vnc_proxy_get_stats();
    String page;
    page.reserve(6400);
    append_page_start(page, "Scope");
    append_nav(page, "/scope");
    append_summary_cards(page);

    page += F("<section class='card'><h2>Scope reachability</h2><form method='post' action='/save'>"
              "<input type='hidden' name='section' value='scope'>"
              "<div class='grid'>"
              "<div class='field'><label>Scope IP</label><input type='text' name='scope_ip' value='");
    page += ip_to_str(g_config.scope_ip);
    page += F("'></div><div class='field'><label>Probe TCP Port</label><input type='text' name='scope_port' value='");
    page += String((unsigned)g_config.scope_port);
    page += F("'></div><div class='field'><label>Connect Timeout (ms)</label><input type='text' name='scope_connect_timeout_ms' value='");
    page += String((unsigned)g_config.scope_connect_timeout_ms);
    page += F("'></div><div class='field'><label>Probe Interval (ms)</label><input type='text' name='scope_probe_interval_ms' value='");
    page += String((unsigned)g_config.scope_probe_interval_ms);
    page += F("'></div><div class='field'><label>VXI Session Timeout (ms)</label><input type='text' name='vxi_session_timeout_ms' value='");
    page += String((unsigned)g_config.vxi_session_timeout_ms);
    page += F("'></div></div><small>The scope IP must stay in the same subnet as the dedicated LAN. Reachability probes and Bode/VXI services use that dedicated LAN whenever Ethernet is up.</small><div class='actions'><button class='btn' type='submit'>Save</button></div></form></section>");

    page += F("<section class='card'><h2>Scope HTTP Proxy</h2><form method='post' action='/save'>"
              "<input type='hidden' name='section' value='scope_proxy'>"
              "<div class='field'><label><input type='checkbox' name='scope_http_proxy_enable' value='1'");
    if (g_config.scope_http_proxy_enable) page += F(" checked");
    page += F("> Enable HTTP/TCP proxy on ESP32 port ");
    page += String((unsigned)scope_http_proxy_listen_port());
    page += F("</label><small>This proxy publishes the oscilloscope web UI at http://ESP32_STA_IP:");
    page += String((unsigned)scope_http_proxy_listen_port());
    page += F("/ and forwards TCP traffic directly to scope_ip:");
    page += String((unsigned)SCOPE_HTTP_PROXY_TARGET_PORT);
    page += F(". No HTTP rewriting is performed unless the logs show that compatibility handling is required.</small></div><table>"
              "<tr><td>Support</td><td>");
    page += proxy_stats->supported ? F("yes") : F("no in this build");
    page += F("</td></tr><tr><td>Status</td><td>");
    page += scope_proxy_status_text();
    page += F("</td></tr><tr><td>noVNC proxy</td><td>");
    page += scope_vnc_proxy_status_text();
    page += F("</td></tr><tr><td>HTTP target</td><td>");
    page += ip_to_str(g_config.scope_ip);
    page += F(":");
    page += String((unsigned)SCOPE_HTTP_PROXY_TARGET_PORT);
    page += F("</td></tr><tr><td>noVNC target</td><td>");
    page += ip_to_str(g_config.scope_ip);
    page += F(":");
    page += String((unsigned)SCOPE_VNC_PROXY_TARGET_PORT);
    page += F("</td></tr><tr><td>HTTP proxy active/max</td><td>");
    page += String((unsigned)proxy_stats->active_connections);
    page += F("/");
    page += String((unsigned)proxy_stats->max_connections);
    page += F("</td></tr><tr><td>HTTP proxy listen port</td><td>");
    page += String((unsigned)scope_http_proxy_listen_port());
    page += F("</td></tr><tr><td>noVNC active/max</td><td>");
    page += String((unsigned)vnc_stats->active_connections);
    page += F("/");
    page += String((unsigned)vnc_stats->max_connections);
    page += F("</td></tr><tr><td>noVNC clients</td><td>");
    page += String((unsigned)vnc_stats->active_connections);
    page += F("</td></tr><tr><td>Total active proxy clients</td><td>");
    page += String((unsigned)(proxy_stats->active_connections + vnc_stats->active_connections));
    page += F("</td></tr><tr><td>HTTP proxy accepted</td><td>");
    page += String((unsigned long)proxy_stats->accepted_connections);
    page += F("</td></tr><tr><td>noVNC accepted</td><td>");
    page += String((unsigned long)vnc_stats->accepted_connections);
    page += F("</td></tr><tr><td>HTTP proxy failed connects</td><td>");
    page += String((unsigned long)proxy_stats->failed_connects);
    page += F("</td></tr><tr><td>noVNC failed connects</td><td>");
    page += String((unsigned long)vnc_stats->failed_connects);
    page += F("</td></tr><tr><td>noVNC bytes client->scope</td><td>");
    page += String((unsigned long)vnc_stats->total_client_to_scope_bytes);
    page += F("</td></tr><tr><td>noVNC bytes scope->client</td><td>");
    page += String((unsigned long)vnc_stats->total_scope_to_client_bytes);
    page += F("</td></tr><tr><td>Compatibility hints</td><td>");
    page += scope_proxy_hint_text();
    page += F("</td></tr><tr><td>noVNC hints</td><td>");
    page += scope_vnc_proxy_hint_text();
    page += F("</td></tr><tr><td>Last error</td><td>");
    append_html_escaped(page, proxy_stats->last_error);
    page += F("</td></tr><tr><td>noVNC last error</td><td>");
    append_html_escaped(page, vnc_stats->last_error);
    page += F("</td></tr></table><small>HTTP proxy and noVNC/WebSocket proxy counters are independent. A non-zero noVNC client count with zero HTTP proxy clients is valid.</small><div class='actions'><button class='btn' type='submit'>Save Proxy</button></div></form></section>");

    append_page_end(page);
    send_page(page);
}

static void handle_fy_get(void)
{
    static const char *const modes[] = { "8N1", "8N2", "8E1", "8E2", "8O1", "8O2", "7E1", "7E2", "7O1", "7O2" };
    String page;
    page.reserve(6200);
    append_page_start(page, "FY6900");
    append_nav(page, "/fy6900");
    append_summary_cards(page);

    page += F("<section class='card'><h2>UART2 to FY6900</h2><form method='post' action='/save'>"
              "<input type='hidden' name='section' value='fy6900'>"
              "<div class='grid'><div class='field'><label>Baud Rate</label><input type='text' name='awg_baud' value='");
    page += String((unsigned long)g_config.awg_baud);
    page += F("'></div><div class='field'><label>Serial Mode</label><select name='awg_serial_mode'>");
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
        page += F("<option value='");
        page += modes[i];
        page += F("'");
        if (strcmp(g_config.awg_serial_mode, modes[i]) == 0) page += F(" selected");
        page += F(">");
        page += modes[i];
        page += F("</option>");
    }
    page += F("</select></div><div class='field'><label>Serial Timeout (ms)</label><input type='text' name='awg_serial_timeout_ms' value='");
    page += String((unsigned)g_config.awg_serial_timeout_ms);
    page += F("'></div></div><small>UART2 pin map: IO17 = TXD2 to FY6900 RX, IO5 = RXD2 from FY6900 TX, with a shared ground.</small>"
              "<div class='actions'><button class='btn' type='submit'>Save</button></div></form></section>");

    page += F("<section class='card'><h2>FY6900 runtime summary</h2><table>"
              "<tr><td>Configured serial mode</td><td>");
    page += g_config.awg_serial_mode;
    page += F("</td></tr><tr><td>Runtime serial mode</td><td>");
    page += fy_get_serial_mode();
    page += F("</td></tr><tr><td>Serial2 initialized</td><td>");
    page += fy_serial_initialized() ? F("yes") : F("no");
    page += F("</td></tr><tr><td>Status</td><td>");
    page += fy_status_text();
    page += F("</td></tr></table><small>The live UART2 runtime uses the current FY runtime config. In the final release build the saved FY settings are applied immediately after Save.</small></section>");

#if FW_ENABLE_MANUAL_FY_TEST
    page += F("<section class='card'><h2>Manual Safe FY Test</h2><table>"
              "<tr><td>Mode</td><td>Manual only, nothing runs at boot</td></tr>"
              "<tr><td>Serial2 initialized</td><td>");
    page += fy_serial_initialized() ? F("yes") : F("no");
    page += F("</td></tr><tr><td>Last manual test</td><td>");
    append_html_escaped(page, fy_last_manual_test_summary());
    page += F("</td></tr></table><p class='muted'>This test initializes UART2 only on demand and runs the read-only UMO probe over 115200 baud with LF line termination.</p>"
              "<div class='actions'><a class='btn warn' href='/fy-test?run=yes'>Run UMO Probe</a></div></section>");
#endif

    append_fy_final_test_section(page);
    append_fy_diag_section(page);

    append_page_end(page);
    send_page(page);
}

static void handle_bode_get(void)
{
    String page;
    page.reserve(4800);
    append_page_start(page, "Bode");
    append_nav(page, "/bode");
    append_summary_cards(page);

    page += F("<section class='card'><h2>Bode parameters</h2><form method='post' action='/save'>"
              "<input type='hidden' name='section' value='bode'>"
              "<div class='grid'><div class='field'><label>Friendly Name</label><input type='text' name='friendly_name' maxlength='24' value='");
    page += g_config.friendly_name;
    page += F("'></div><div class='field'><label>IDN Model Name</label><input type='text' name='idn_response_name' maxlength='16' value='");
    page += g_config.idn_response_name;
    page += F("'></div><div class='field'><label>FY Firmware Family</label><select name='awg_firmware_family'>"
              "<option value='0'");
    if (g_config.awg_firmware_family == AWG_FW_FAMILY_OLDER) page += F(" selected");
    page += F(">older FY6800 / early FY6900</option><option value='1'");
    if (g_config.awg_firmware_family == AWG_FW_FAMILY_LATER_FY6900) page += F(" selected");
    page += F(">later FY6900</option></select></div><div class='field'><label>Auto Output-Off Timeout (ms)</label><input type='text' name='auto_output_off_timeout_ms' value='");
    page += String((unsigned)g_config.auto_output_off_timeout_ms);
    page += F("'></div></div><small>Friendly Name is stored for UI/diagnostics only. IDN Model Name affects the live *IDN? response, FY Firmware Family changes FY command formatting, and Auto Output-Off Timeout is enforced by the VXI runtime. VXI Session Timeout remains configured in the Scope tab.</small><div class='actions'><button class='btn' type='submit'>Save</button></div></form></section>");

    append_page_end(page);
    send_page(page);
}

static void handle_diag_get(void)
{
    const WebUiServerStats *web_stats = webconfig_get_stats();
    String page;
    page.reserve(9400);
    append_page_start(page, "Diagnostics");
    append_nav(page, "/diag");
    append_summary_cards(page);
    page += F("<script>scheduleAutoRefresh(5000);</script><p class='muted'>Diagnostics auto-refresh every 5 seconds so runtime counters stay current.</p>");

    page += F("<section class='card'><h2>System</h2><table>");
    page += F("<tr><td>Uptime</td><td>");
    page += String((unsigned long)millis());
    page += F(" ms</td></tr><tr><td>Heap</td><td>");
    page += String((unsigned long)ESP.getFreeHeap());
    page += F(" bytes</td></tr><tr><td>Reset reason</td><td>");
    page += reset_reason_text();
    page += F("</td></tr><tr><td>Runtime status</td><td>");
    page += runtime_status_detail();
    page += F("</td></tr><tr><td>Last network fail</td><td>");
    page += runtime_fail_reason_label(runtime_net_last_fail_reason());
    page += F("</td></tr><tr><td>STA retries</td><td>");
    page += String((unsigned)runtime_net_sta_retry_count());
    page += F("/");
    page += String((unsigned)runtime_net_sta_retry_limit());
    page += F("</td></tr><tr><td>STA retry exhausted</td><td>");
    page += runtime_net_sta_retry_exhausted() ? F("yes") : F("no");
    page += F("</td></tr><tr><td>AP SSID</td><td>");
    append_html_escaped(page, g_config.ap_ssid);
    page += F("</td></tr><tr><td>Recovery AP enabled</td><td>");
    page += g_config.recovery_ap_enable ? F("on") : F("off");
    page += F("</td></tr><tr><td>MAC</td><td>");
    append_html_escaped(page, runtime_net_mac());
    page += F("</td></tr><tr><td>LAN link</td><td>");
    page += runtime_net_lan_link_up() ? F("up") : F("down");
    page += F("</td></tr><tr><td>LAN IP</td><td>");
    page += runtime_net_lan_ip().toString();
    page += F("</td></tr><tr><td>WiFi STA IP</td><td>");
    page += runtime_net_sta_ip().toString();
    page += F("</td></tr><tr><td>AP IP</td><td>");
    page += runtime_net_ap_ip().toString();
    page += F("</td></tr><tr><td>WiFi RSSI</td><td>");
    if (runtime_net_sta_connected()) {
        page += String((long)runtime_net_rssi());
        page += F(" dBm");
    } else {
        page += F("n/a");
    }
    page += F("</td></tr><tr><td>Recovery AP clients</td><td>");
    page += String((unsigned)runtime_net_ap_client_count());
    page += F("</td></tr><tr><td>Web UI clients</td><td>");
    page += String((unsigned)web_stats->active_clients);
    page += F("/");
    page += String((unsigned)web_stats->max_clients);
    page += F("</td></tr></table></section>");

    page += F("<section class='card'><h2>Time / NTP</h2><table>"
              "<tr><td>Time status</td><td>");
    page += runtime_net_time_status_text();
    page += F("</td></tr><tr><td>LAN NTP server</td><td>");
    page += runtime_net_ntp_server_running() ? F("on") : F("off");
    page += F("</td></tr><tr><td>NTP LAN only</td><td>");
    page += runtime_net_ntp_lan_only() ? F("yes") : F("no");
    page += F("</td></tr><tr><td>LAN subnet restriction</td><td>");
    page += runtime_net_ntp_subnet_restriction_active() ? F("yes") : F("no");
    page += F("</td></tr><tr><td>NTP rate limit</td><td>");
    page += runtime_net_ntp_rate_limit_active() ? F("on") : F("off");
    page += F(" (");
    page += String((unsigned)runtime_net_ntp_rate_limit_per_ip());
    page += F(" per IP / ");
    page += String((unsigned)runtime_net_ntp_rate_limit_global());
    page += F(" global per ");
    page += String((unsigned)NTP_RATE_LIMIT_WINDOW_MS);
    page += F(" ms)");
    page += F("</td></tr><tr><td>Configured upstream</td><td>");
    page += g_config.ntp_server;
    page += F("</td></tr><tr><td>NTP requests served</td><td>");
    page += String((unsigned long)runtime_net_ntp_request_count());
    page += F("</td></tr><tr><td>NTP policy drops</td><td>");
    page += String((unsigned long)runtime_net_ntp_policy_drop_count());
    page += F("</td></tr><tr><td>NTP rate-limit drops</td><td>");
    page += String((unsigned long)runtime_net_ntp_rate_limit_drop_count());
    page += F("</td></tr><tr><td>Last NTP served</td><td>");
    page += String((unsigned long)runtime_net_ntp_last_served_ms());
    page += F(" ms</td></tr></table></section>");

    page += F("<section class='card'><h2>Scope status</h2><table>"
              "<tr><td>Probe enabled</td><td>");
    page += runtime_net_scope_probe_available() ? F("yes") : F("no");
    page += F("</td></tr><tr><td>Reachable</td><td>");
    page += runtime_net_scope_reachable() ? F("yes") : F("no");
    page += F("</td></tr><tr><td>Last check</td><td>");
    page += String((unsigned long)runtime_net_scope_last_check_ms());
    page += F(" ms</td></tr><tr><td>Last success</td><td>");
    page += String((unsigned long)runtime_net_scope_last_success_ms());
    page += F(" ms</td></tr><tr><td>HTTP proxy</td><td>");
    page += scope_proxy_status_text();
    page += F("</td></tr><tr><td>HTTP proxy clients</td><td>");
    page += String((unsigned)scope_http_proxy_get_stats()->active_connections);
    page += F("/");
    page += String((unsigned)scope_http_proxy_get_stats()->max_connections);
    page += F("</td></tr><tr><td>Total active proxy clients</td><td>");
    page += String((unsigned)(scope_http_proxy_get_stats()->active_connections + scope_vnc_proxy_get_stats()->active_connections));
    page += F("</td></tr><tr><td>HTTP proxy listen port</td><td>");
    page += String((unsigned)scope_http_proxy_listen_port());
    page += F("</td></tr><tr><td>noVNC proxy</td><td>");
    page += scope_vnc_proxy_status_text();
    page += F("</td></tr><tr><td>noVNC clients</td><td>");
    page += String((unsigned)scope_vnc_proxy_get_stats()->active_connections);
    page += F("/");
    page += String((unsigned)scope_vnc_proxy_get_stats()->max_connections);
    page += F("</td></tr><tr><td>noVNC listen port</td><td>");
    page += String((unsigned)scope_vnc_proxy_listen_port());
    page += F("</td></tr><tr><td>noVNC target</td><td>");
    page += ip_to_str(g_config.scope_ip);
    page += F(":");
    page += String((unsigned)SCOPE_VNC_PROXY_TARGET_PORT);
    page += F("</td></tr><tr><td>noVNC bytes client->scope</td><td>");
    page += String((unsigned long)scope_vnc_proxy_get_stats()->total_client_to_scope_bytes);
    page += F("</td></tr><tr><td>noVNC bytes scope->client</td><td>");
    page += String((unsigned long)scope_vnc_proxy_get_stats()->total_scope_to_client_bytes);
    page += F("</td></tr><tr><td>HTTP target</td><td>");
    page += ip_to_str(g_config.scope_ip);
    page += F(":");
    page += String((unsigned)SCOPE_HTTP_PROXY_TARGET_PORT);
    page += F("</td></tr><tr><td>Compatibility hints</td><td>");
    page += scope_proxy_hint_text();
    page += F("</td></tr><tr><td>noVNC hints</td><td>");
    page += scope_vnc_proxy_hint_text();
    page += F("</td></tr><tr><td>Last proxy error</td><td>");
    append_html_escaped(page, scope_http_proxy_get_stats()->last_error);
    page += F("</td></tr><tr><td>Last noVNC proxy error</td><td>");
    append_html_escaped(page, scope_vnc_proxy_get_stats()->last_error);
    page += F("</td></tr></table></section>");

    page += F("<section class='card'><h2>FY6900 status</h2><table>"
              "<tr><td>Enabled</td><td>");
    page += fy_is_enabled() ? F("yes") : F("no");
    page += F("</td></tr><tr><td>Baud</td><td>");
    page += String((unsigned long)fy_get_baud());
    page += F("</td></tr><tr><td>Serial mode</td><td>");
    page += fy_get_serial_mode();
    page += F("</td></tr><tr><td>Status</td><td>");
    page += fy_status_text();
    page += F("</td></tr><tr><td>Timeout</td><td>");
    page += String((unsigned)fy_get_timeout_ms());
    page += F(" ms</td></tr></table></section>");

#if FW_ENABLE_MANUAL_FY_TEST
    page += F("<section class='card'><h2>Manual FY Test</h2><table>"
              "<tr><td>Serial2 initialized</td><td>");
    page += fy_serial_initialized() ? F("yes") : F("no");
    page += F("</td></tr><tr><td>Last manual test</td><td>");
    append_html_escaped(page, fy_last_manual_test_summary());
    page += F("</td></tr></table><div class='actions'><a class='btn warn' href='/fy-test?run=yes'>Run UMO Probe</a></div></section>");
#endif

    append_fy_final_test_section(page);
    append_fy_diag_section(page);

    append_protocol_diag_section(page);
    append_page_end(page);
    send_page(page);
}

static void handle_fy_test(void)
{
#if !FW_ENABLE_MANUAL_FY_TEST
    send_message_page("FY test unavailable",
        String(F("This variant does not expose the manual FY test endpoint.")),
        true, "/fy6900", "Back", false);
#else
    if (s_server.arg("run") != "yes") {
        send_message_page("Manual FY test",
            String(F("Run the read-only UMO probe now? This initializes UART2 only on demand and does not run the normal FY work sequence.")),
            false, "/fy6900", "Cancel", false);
        return;
    }

    String before_status = fy_status_text();
    bool before_serial = fy_serial_initialized();
    char summary[192];
    bool ok = fy_manual_safe_test(summary, sizeof(summary));

    String message;
    message.reserve(640);
    message += ok
        ? F("UMO probe completed.<br><br>")
        : F("UMO probe failed safely.<br><br>");
    message += F("Variant: ");
    message += FW_VARIANT_NAME;
    message += F("<br>Requested manually: yes");
    message += F("<br>Serial2 before: ");
    message += before_serial ? F("yes") : F("no");
    message += F("<br>Status before: ");
    append_html_escaped(message, before_status.c_str());
    message += F("<br>Serial2 after: ");
    message += fy_serial_initialized() ? F("yes") : F("no");
    message += F("<br>Status after: ");
    append_html_escaped(message, fy_status_text());
    message += F("<br>Summary: ");
    append_html_escaped(message, summary);

    send_message_page(ok ? "FY test completed" : "FY test safe-fail",
        message, !ok, "/fy6900", "Back to FY6900", false);
#endif
}

static void handle_fy_final_umo(void)
{
#if !FW_ENABLE_FY_FINAL_TEST
    send_message_page("FY final test unavailable",
        String(F("This variant does not expose the final FY test flow.")),
        true, "/fy6900", "Back", false);
#else
    if (s_server.arg("run") != "yes") {
        send_message_page("Re-test UMO",
            String(F("Run the safe read-only UMO probe now? This does not execute the functional SCPI sequence.")),
            false, "/fy6900", "Cancel", false);
        return;
    }

    char summary[192];
    bool ok = fy_manual_safe_test(summary, sizeof(summary));
    String message;
    message.reserve(2200);
    message += ok
        ? F("UMO probe completed.<br><br>")
        : F("UMO probe failed safely.<br><br>");
    message += F("Summary: ");
    append_html_escaped(message, summary);
    message += F("<br><br>");
    append_fy_diag_snapshot_html(message);
    send_message_page(ok ? "UMO re-test completed" : "UMO re-test safe-fail",
        message, !ok, "/fy6900", "Back to FY6900", false);
#endif
}

static void handle_fy_final_test(void)
{
#if !FW_ENABLE_FY_FINAL_TEST
    send_message_page("FY final test unavailable",
        String(F("This variant does not expose the final FY test flow.")),
        true, "/fy6900", "Back", false);
#else
    if (s_server.arg("run") != "yes") {
        send_message_page("Minimal FY functional test",
            String(F("Run the safe near-final FY test now? This initializes UART2 on demand, verifies UMO, parses the payload, then runs a minimal SCPI write/query sequence with outputs kept OFF.")),
            false, "/fy6900", "Cancel", false);
        return;
    }

    char summary[224];
    bool ok = fy_functional_test_run(g_config.awg_serial_timeout_ms, summary, sizeof(summary));
    String message;
    message.reserve(2600);
    message += ok
        ? F("Minimal FY functional test completed.<br><br>")
        : F("Minimal FY functional test failed safely.<br><br>");
    message += F("Summary: ");
    append_html_escaped(message, summary);
    message += F("<br><br>");
    append_fy_diag_snapshot_html(message);
    send_message_page(ok ? "FY final test completed" : "FY final test safe-fail",
        message, !ok, "/fy6900", "Back to FY6900", false);
#endif
}

static void handle_fy_final_reset(void)
{
#if !FW_ENABLE_FY_FINAL_TEST
    send_message_page("FY final test unavailable",
        String(F("This variant does not expose the final FY test flow.")),
        true, "/fy6900", "Back", false);
#else
    fy_functional_reset_state();
    String message;
    message.reserve(2200);
    message += F("FY software state reset completed.<br><br>");
    append_fy_diag_snapshot_html(message);
    send_message_page("FY software state reset", message, false, "/fy6900", "Back to FY6900", false);
#endif
}

static void handle_fy_diag_summary(void)
{
#if !FW_ENABLE_FY_SERIAL_DIAG
    send_message_page("FY diag unavailable",
        String(F("This variant does not expose FY serial diagnostics.")),
        true, "/fy6900", "Back", false);
#else
    String message;
    message.reserve(2200);
    append_fy_diag_snapshot_html(message);
    send_message_page("FY diag summary", message, false, "/fy6900", "Back to FY6900", false);
#endif
}

static void handle_fy_diag_init(void)
{
#if !FW_ENABLE_FY_SERIAL_DIAG
    handle_fy_diag_summary();
#else
    bool ok = fy_diag_uart_init();
    String message;
    message.reserve(1800);
    message += ok ? F("Serial2 initialized for FY diagnostics.<br><br>") : F("Serial2 init failed.<br><br>");
    append_fy_diag_snapshot_html(message);
    send_message_page(ok ? "FY diag init" : "FY diag init failed",
        message, !ok, "/fy6900", "Back to FY6900", false);
#endif
}

static void handle_fy_diag_reinit(void)
{
#if !FW_ENABLE_FY_SERIAL_DIAG
    handle_fy_diag_summary();
#else
    bool ok = fy_diag_uart_reinit();
    String message;
    message.reserve(1800);
    message += ok ? F("UART2 reinitialized for FY diagnostics.<br><br>") : F("UART2 reinit failed.<br><br>");
    append_fy_diag_snapshot_html(message);
    send_message_page(ok ? "FY diag reinit" : "FY diag reinit failed",
        message, !ok, "/fy6900", "Back to FY6900", false);
#endif
}

static void handle_fy_diag_deinit(void)
{
#if !FW_ENABLE_FY_SERIAL_DIAG
    handle_fy_diag_summary();
#else
    fy_diag_uart_deinit();
    String message;
    message.reserve(1800);
    message += F("Serial2 deinitialized for FY diagnostics.<br><br>");
    append_fy_diag_snapshot_html(message);
    send_message_page("FY diag deinit", message, false, "/fy6900", "Back to FY6900", false);
#endif
}

static void handle_fy_diag_flush(void)
{
#if !FW_ENABLE_FY_SERIAL_DIAG
    handle_fy_diag_summary();
#else
    size_t count = fy_diag_flush_rx();
    String message;
    message.reserve(1800);
    message += F("RX flush completed. Bytes discarded: ");
    message += String((unsigned long)count);
    message += F("<br><br>");
    append_fy_diag_snapshot_html(message);
    send_message_page("FY diag flush", message, false, "/fy6900", "Back to FY6900", false);
#endif
}

static void handle_fy_diag_read(void)
{
#if !FW_ENABLE_FY_SERIAL_DIAG
    handle_fy_diag_summary();
#else
    uint16_t timeout_ms = 0;
    if (s_server.hasArg("timeout_ms")) {
        (void)parse_u16(s_server.arg("timeout_ms"), &timeout_ms);
    }
    int rx_len = fy_diag_read_raw(timeout_ms);
    String message;
    message.reserve(1800);
    message += F("Raw FY read completed. RX length: ");
    message += String(rx_len);
    message += F("<br><br>");
    append_fy_diag_snapshot_html(message);
    send_message_page(rx_len >= 0 ? "FY diag read" : "FY diag read failed",
        message, rx_len < 0, "/fy6900", "Back to FY6900", false);
#endif
}

static void handle_fy_diag_probe(void)
{
#if !FW_ENABLE_FY_SERIAL_DIAG
    handle_fy_diag_summary();
#else
    uint16_t timeout_ms = 0;
    char summary[192];
    if (s_server.hasArg("timeout_ms")) {
        (void)parse_u16(s_server.arg("timeout_ms"), &timeout_ms);
    }
    bool ok = fy_diag_probe_protocol(timeout_ms, summary, sizeof(summary));
    String message;
    message.reserve(2200);
    message += ok ? F("FY protocol probe completed.<br>") : F("FY protocol probe failed safely.<br>");
    message += F("Summary: ");
    append_html_escaped(message, summary);
    message += F("<br><br>");
    append_fy_diag_snapshot_html(message);
    send_message_page(ok ? "FY diag probe" : "FY diag probe safe-fail",
        message, !ok, "/fy6900", "Back to FY6900", false);
#endif
}

static void handle_fy_diag_compare(void)
{
#if !FW_ENABLE_FY_SERIAL_DIAG
    handle_fy_diag_summary();
#else
    uint16_t timeout_ms = 0;
    char summary[256];
    if (s_server.hasArg("timeout_ms")) {
        (void)parse_u16(s_server.arg("timeout_ms"), &timeout_ms);
    }
    bool ok = fy_diag_compare_8n2_vs_8n1(timeout_ms, summary, sizeof(summary));
    String message;
    message.reserve(2400);
    message += ok ? F("8N2/8N1 comparison completed.<br>") : F("8N2/8N1 comparison did not produce a valid payload.<br>");
    message += F("Summary: ");
    append_html_escaped(message, summary);
    message += F("<br><br>");
    append_fy_diag_snapshot_html(message);
    send_message_page(ok ? "FY diag compare" : "FY diag compare inconclusive",
        message, !ok, "/fy6900", "Back to FY6900", false);
#endif
}

static void handle_fy_diag_send(void)
{
#if !FW_ENABLE_FY_SERIAL_DIAG
    handle_fy_diag_summary();
#else
    uint16_t timeout_ms = 0;
    String raw_cmd = s_server.arg("raw_cmd");
    bool append_newline = s_server.arg("append_newline") == "1";
    if (s_server.hasArg("timeout_ms")) {
        (void)parse_u16(s_server.arg("timeout_ms"), &timeout_ms);
    }

    int rx_len = fy_diag_send_raw(raw_cmd.c_str(), append_newline, timeout_ms);
    String message;
    message.reserve(2200);
    message += F("Raw FY command sent.<br>Command: <code>");
    append_html_escaped(message, raw_cmd.c_str());
    message += F("</code><br>Read timeout: ");
    message += String((unsigned)timeout_ms);
    message += F(" ms<br>RX length: ");
    message += String(rx_len);
    message += F("<br><br>");
    append_fy_diag_snapshot_html(message);
    send_message_page(rx_len >= 0 ? "FY diag send" : "FY diag send failed",
        message, rx_len < 0, "/fy6900", "Back to FY6900", false);
#endif
}

static void handle_save_get(void)
{
    send_message_page("Save", String(F("Use the forms on the configuration pages.")), false,
        "/", "Back", false);
}

static void handle_save_post(void)
{
    EspConfig next = g_config;
    EspConfig previous = g_config;
    String section = s_server.arg("section");
    String error;
    String warning;
    bool ok = true;
    bool network_changed;
    bool fy_changed;

    if (section == "network") {
        uint8_t ip[4], mask[4], gw[4], dns1[4], dns2[4];
        String hostname = s_server.arg("device_hostname");
        String ntp_server = s_server.arg("ntp_server");
        String ssid = s_server.arg("wifi_ssid");
        String password = s_server.arg("wifi_password");
        String ap_ssid = s_server.arg("ap_ssid");
        String ap_password = s_server.arg("ap_password");
        bool use_dhcp = s_server.arg("dhcp") == "1";

        memcpy(ip, next.ip, 4u);
        memcpy(mask, next.mask, 4u);
        memcpy(gw, next.gw, 4u);
        memcpy(dns1, next.dns, 4u);
        memcpy(dns2, next.dns2, 4u);

        if (!is_safe_hostname(hostname)) { ok = false; error += F("Invalid hostname. "); }
        if (!is_safe_idn_name(ntp_server) || ntp_server.length() > sizeof(next.ntp_server) - 1) {
            ok = false; error += F("Invalid NTP server. ");
        }
        if (ssid.length() > 0 && !is_safe_label_text(ssid, sizeof(next.wifi_ssid) - 1, true)) {
            ok = false; error += F("SSID must be printable. ");
        }
        if (ssid.length() == 0 && password.length() > 0) {
            ok = false; error += F("Password requires a non-empty SSID. ");
        }
        if (password.length() > 0 && (password.length() < 8 || password.length() > 63 || !is_safe_label_text(password, sizeof(next.wifi_password) - 1, true))) {
            ok = false; error += F("Password must be empty or 8..63 printable chars. ");
        }
        if (ap_ssid.length() == 0 || !is_safe_label_text(ap_ssid, sizeof(next.ap_ssid) - 1, true)) {
            ok = false; error += F("AP SSID must be non-empty and printable. ");
        }
        if (!is_valid_ap_password_input(ap_password)) {
#if FW_FORCE_RECOVERY_AP
            ok = false; error += F("AP password must be empty or 8..63 printable chars in this safe build. ");
#else
            ap_password = DEF_RECOVERY_AP_PASSWORD;
            warning += F(" AP password was invalid for SoftAP and was reset to the defensive default.");
#endif
        }
        if (!use_dhcp) {
            if (!str_to_ip(s_server.arg("ip"), ip)) { ok = false; error += F("Invalid local IP. "); }
            if (!str_to_ip(s_server.arg("mask"), mask) || !subnet_is_valid(mask)) { ok = false; error += F("Invalid subnet mask. "); }
            if (!str_to_ip(s_server.arg("gw"), gw)) { ok = false; error += F("Invalid gateway. "); }
            if (!str_to_ip(s_server.arg("dns"), dns1) || !nonzero_ip_is_valid(dns1)) { ok = false; error += F("Invalid DNS1. "); }
            if (s_server.hasArg("dns2") && s_server.arg("dns2").length() > 0) {
                if (!str_to_ip(s_server.arg("dns2"), dns2) || !nonzero_ip_is_valid(dns2)) {
                    ok = false; error += F("Invalid DNS2. ");
                }
            }
            if (ok && (!host_is_valid_for_mask(ip, mask) || !host_is_valid_for_mask(gw, mask) || !same_subnet(ip, gw, mask))) {
                ok = false; error += F("Static IP and gateway must be valid hosts in the same subnet. ");
            }
            if (ok && same_subnet(ip, next.lan_ip, next.lan_mask)) {
                ok = false; error += F("WiFi STA static subnet must differ from LAN subnet. ");
            }
        }

        if (ok) {
            next.use_dhcp = use_dhcp ? 1u : 0u;
            next.recovery_ap_enable = s_server.hasArg("recovery_ap_enable") ? 1u : 0u;
            strncpy(next.device_hostname, hostname.c_str(), sizeof(next.device_hostname) - 1);
            next.device_hostname[sizeof(next.device_hostname) - 1] = '\0';
            strncpy(next.ntp_server, ntp_server.c_str(), sizeof(next.ntp_server) - 1);
            next.ntp_server[sizeof(next.ntp_server) - 1] = '\0';
            strncpy(next.wifi_ssid, ssid.c_str(), sizeof(next.wifi_ssid) - 1);
            next.wifi_ssid[sizeof(next.wifi_ssid) - 1] = '\0';
            strncpy(next.wifi_password, password.c_str(), sizeof(next.wifi_password) - 1);
            next.wifi_password[sizeof(next.wifi_password) - 1] = '\0';
            strncpy(next.ap_ssid, ap_ssid.c_str(), sizeof(next.ap_ssid) - 1);
            next.ap_ssid[sizeof(next.ap_ssid) - 1] = '\0';
            strncpy(next.ap_password, ap_password.c_str(), sizeof(next.ap_password) - 1);
            next.ap_password[sizeof(next.ap_password) - 1] = '\0';
            if (ssid.length() == 0) {
                next.wifi_password[0] = '\0';
            }
            if (!use_dhcp) {
                memcpy(next.ip, ip, 4u);
                memcpy(next.mask, mask, 4u);
                memcpy(next.gw, gw, 4u);
                memcpy(next.dns, dns1, 4u);
                memcpy(next.dns2, dns2, 4u);
            }
        }
    } else if (section == "lan") {
        uint8_t lan_ip[4], lan_mask[4];

        if (!str_to_ip(s_server.arg("lan_ip"), lan_ip)) {
            ok = false; error += F("Invalid LAN IP. ");
        }
        if (!str_to_ip(s_server.arg("lan_mask"), lan_mask) || !subnet_is_valid(lan_mask)) {
            ok = false; error += F("Invalid LAN mask. ");
        }
        if (ok && !host_is_valid_for_mask(lan_ip, lan_mask)) {
            ok = false; error += F("LAN IP must be a valid host in its subnet. ");
        }
        if (ok && !next.use_dhcp && same_subnet(next.ip, lan_ip, lan_mask)) {
            ok = false; error += F("LAN subnet must differ from WiFi STA static subnet. ");
        }
        if (ok) {
            memcpy(next.lan_ip, lan_ip, 4u);
            memcpy(next.lan_mask, lan_mask, 4u);
            if (!same_subnet(next.scope_ip, lan_ip, lan_mask) || ip_to_u32(next.scope_ip) == ip_to_u32(lan_ip)) {
                warning += F(" Scope IP was outside the new LAN subnet and will be normalized on save.");
            }
        }
    } else if (section == "scope") {
        uint8_t scope_ip[4];
        uint16_t scope_port = 0;
        uint16_t connect_timeout = 0;
        uint16_t probe_interval = 0;
        uint16_t session_timeout = 0;

        if (!str_to_ip(s_server.arg("scope_ip"), scope_ip) || !nonzero_ip_is_valid(scope_ip)) {
            ok = false; error += F("Invalid scope IP. ");
        }
        if (ok && (!same_subnet(scope_ip, next.lan_ip, next.lan_mask)
                || ip_to_u32(scope_ip) == ip_to_u32(next.lan_ip))) {
            ok = false; error += F("Scope IP must be a valid LAN host in the current LAN subnet. ");
        }
        if (!parse_u16(s_server.arg("scope_port"), &scope_port) || scope_port == 0u) {
            ok = false; error += F("Invalid scope port. ");
        }
        if (!parse_u16(s_server.arg("scope_connect_timeout_ms"), &connect_timeout)
                || connect_timeout < MIN_SCOPE_TIMEOUT_MS || connect_timeout > MAX_SCOPE_TIMEOUT_MS) {
            ok = false; error += F("Invalid scope connect timeout. ");
        }
        if (!parse_u16(s_server.arg("scope_probe_interval_ms"), &probe_interval)
                || probe_interval < MIN_SCOPE_INTERVAL_MS || probe_interval > MAX_SCOPE_INTERVAL_MS) {
            ok = false; error += F("Invalid scope probe interval. ");
        }
        if (!parse_u16(s_server.arg("vxi_session_timeout_ms"), &session_timeout)
                || session_timeout < MIN_VXI_TIMEOUT_MS || session_timeout > MAX_VXI_TIMEOUT_MS) {
            ok = false; error += F("Invalid VXI session timeout. ");
        }
        if (ok) {
            memcpy(next.scope_ip, scope_ip, 4u);
            next.scope_port = scope_port;
            next.scope_connect_timeout_ms = connect_timeout;
            next.scope_probe_interval_ms = probe_interval;
            next.vxi_session_timeout_ms = session_timeout;
        }
    } else if (section == "scope_proxy") {
        next.scope_http_proxy_enable = s_server.hasArg("scope_http_proxy_enable") ? 1u : 0u;
        if (next.scope_http_proxy_enable > 1u) {
            ok = false; error += F("Invalid proxy enable state. ");
        }
    } else if (section == "service_limits") {
        uint32_t parsed_web = 0;
        uint32_t parsed_http = 0;
        uint32_t parsed_vnc = 0;
        uint8_t clamped_web;
        uint8_t clamped_http;
        uint8_t clamped_vnc;

        if (!parse_u32(s_server.arg("max_web_ui_clients"), &parsed_web)) {
            ok = false; error += F("Invalid Web UI client limit. ");
        }
        if (!parse_u32(s_server.arg("max_scope_http_proxy_clients"), &parsed_http)) {
            ok = false; error += F("Invalid HTTP proxy client limit. ");
        }
        if (!parse_u32(s_server.arg("max_scope_vnc_proxy_clients"), &parsed_vnc)) {
            ok = false; error += F("Invalid noVNC client limit. ");
        }

        clamped_web = clamp_web_service_limit(parsed_web);
        clamped_http = clamp_web_service_limit(parsed_http);
        clamped_vnc = clamp_web_service_limit(parsed_vnc);

        if (ok) {
            if (parsed_web != clamped_web) {
                warning += F(" Web UI client limit was clamped to 2..30.");
            }
            if (parsed_http != clamped_http) {
                warning += F(" Scope HTTP proxy client limit was clamped to 2..30.");
            }
            if (parsed_vnc != clamped_vnc) {
                warning += F(" noVNC client limit was clamped to 2..30.");
            }
            next.max_web_ui_clients = clamped_web;
            next.max_scope_http_proxy_clients = clamped_http;
            next.max_scope_vnc_proxy_clients = clamped_vnc;
        }
    } else if (section == "fy6900") {
        uint32_t baud = 0;
        uint16_t timeout = 0;
        String serial_mode = s_server.arg("awg_serial_mode");

        if (!parse_u32(s_server.arg("awg_baud"), &baud) || !fy_is_supported_baud(baud)) {
            ok = false; error += F("Unsupported FY6900 baud rate. ");
        }
        if (!fy_is_supported_serial_mode(serial_mode.c_str())) {
            ok = false; error += F("Unsupported FY6900 serial mode. ");
        }
        if (!parse_u16(s_server.arg("awg_serial_timeout_ms"), &timeout)
                || timeout < MIN_AWG_TIMEOUT_MS || timeout > MAX_AWG_TIMEOUT_MS) {
            ok = false; error += F("Invalid FY6900 timeout. ");
        }
        if (ok) {
            next.awg_baud = baud;
            next.awg_serial_timeout_ms = timeout;
            strncpy(next.awg_serial_mode, serial_mode.c_str(), sizeof(next.awg_serial_mode) - 1);
            next.awg_serial_mode[sizeof(next.awg_serial_mode) - 1] = '\0';
        }
    } else if (section == "bode") {
        uint16_t auto_off = 0;
        uint32_t family = 0;
        String friendly_name = s_server.arg("friendly_name");
        String idn_name = s_server.arg("idn_response_name");

        if (!is_safe_label_text(friendly_name, sizeof(next.friendly_name) - 1, true)) {
            ok = false; error += F("Invalid friendly name. ");
        }
        if (!is_safe_idn_name(idn_name)) {
            ok = false; error += F("Invalid IDN model name. ");
        }
        if (!parse_u32(s_server.arg("awg_firmware_family"), &family)
                || !fy_is_supported_firmware_family((uint8_t)family)) {
            ok = false; error += F("Invalid FY firmware family. ");
        }
        if (!parse_u16(s_server.arg("auto_output_off_timeout_ms"), &auto_off)
                || auto_off < MIN_AUTO_OFF_TIMEOUT_MS || auto_off > MAX_AUTO_OFF_TIMEOUT_MS) {
            ok = false; error += F("Invalid auto output-off timeout. ");
        }
        if (ok) {
            strncpy(next.friendly_name, friendly_name.c_str(), sizeof(next.friendly_name) - 1);
            next.friendly_name[sizeof(next.friendly_name) - 1] = '\0';
            strncpy(next.idn_response_name, idn_name.c_str(), sizeof(next.idn_response_name) - 1);
            next.idn_response_name[sizeof(next.idn_response_name) - 1] = '\0';
            next.awg_firmware_family = (uint8_t)family;
            next.auto_output_off_timeout_ms = auto_off;
        }
    } else {
        ok = false;
        error += F("Unknown save section. ");
    }

    if (!ok) {
        send_message_page("Save failed", error, true, save_back_path(section), "Back", false);
        return;
    }

    network_changed = network_settings_changed(g_config, next);
    fy_changed = fy_settings_changed(g_config, next);
    g_config = next;

    if (!saveConfig()) {
        g_config = previous;
        send_message_page("Save failed", String(F("Preferences/NVS write failed.")), true,
            "/", "Back", false);
        return;
    }
    if (!config_last_save_wrote()) {
        send_message_page("No changes saved",
            String(F("No real changes were detected. Preferences/NVS was not rewritten.")) + warning,
            false,
            save_back_path(section),
            "Back",
            false);
        return;
    }
    if (fy_changed) {
#if FW_ENABLE_MANUAL_FY_TEST
        Serial.printf("[fy manual] config saved without runtime apply in variant=%s\r\n", FW_VARIANT_NAME);
#else
        fy_apply_runtime_config();
#endif
    }

    send_message_page("Configuration saved",
        (network_changed
            ? String(F("Config saved to Preferences. Network changes require a reboot to take effect."))
            : (fy_changed && FW_ENABLE_MANUAL_FY_TEST)
                ? String(F("Config saved to Preferences. FY changes were not applied automatically in this manual safe build; use the FY manual/final test controls to initialize UART2 on demand."))
            : String(F("Config saved to Preferences. Safe runtime updates were applied immediately where possible."))) + warning,
        false,
        save_back_path(section),
        "Back",
        network_changed);
}

static void handle_reboot(void)
{
    if (s_server.arg("confirm") != "yes") {
        send_message_page("Reboot", String(F("Reboot the WT32 now?")), false,
            "/", "Cancel", true);
        return;
    }

    send_message_page("Reboot", String(F("Restarting now...")), false,
        NULL, NULL, false);
    delay(1200);
    ESP.restart();
}

static void handle_factory_reset(void)
{
    if (s_server.arg("confirm") != "yes") {
        send_message_page("Factory reset",
            String(F("This resets the full configuration to WiFi STA defaults plus recovery AP fallback.")),
            false, "/", "Cancel", false);
        return;
    }

    resetConfigToDefaults();
    (void)saveConfig();
    send_message_page("Factory reset", String(F("Defaults restored. The device will reboot now.")),
        false, NULL, NULL, false);
    delay(1200);
    ESP.restart();
}

static void handle_not_found(void)
{
    send_message_page("404", String(F("Requested page does not exist.")), true,
        "/", "Back to status", false);
}

void webconfig_begin(void)
{
    if (s_server_started) return;

    s_server.on("/", HTTP_GET, handle_root);
    s_server.on("/config", HTTP_GET, handle_network_get);
    s_server.on("/network", HTTP_GET, handle_network_get);
    s_server.on("/scope", HTTP_GET, handle_scope_get);
    s_server.on("/fy6900", HTTP_GET, handle_fy_get);
    s_server.on("/fy-test", HTTP_GET, handle_fy_test);
    s_server.on("/fy-final-umo", HTTP_GET, handle_fy_final_umo);
    s_server.on("/fy-final-test", HTTP_GET, handle_fy_final_test);
    s_server.on("/fy-final-reset", HTTP_GET, handle_fy_final_reset);
    s_server.on("/fy-diag-summary", HTTP_GET, handle_fy_diag_summary);
    s_server.on("/fy-diag-init", HTTP_GET, handle_fy_diag_init);
    s_server.on("/fy-diag-reinit", HTTP_GET, handle_fy_diag_reinit);
    s_server.on("/fy-diag-deinit", HTTP_GET, handle_fy_diag_deinit);
    s_server.on("/fy-diag-flush", HTTP_GET, handle_fy_diag_flush);
    s_server.on("/fy-diag-read", HTTP_GET, handle_fy_diag_read);
    s_server.on("/fy-diag-probe", HTTP_GET, handle_fy_diag_probe);
    s_server.on("/fy-diag-compare", HTTP_GET, handle_fy_diag_compare);
    s_server.on("/fy-diag-send", HTTP_POST, handle_fy_diag_send);
    s_server.on("/bode", HTTP_GET, handle_bode_get);
    s_server.on("/diag", HTTP_GET, handle_diag_get);
    s_server.on("/save", HTTP_GET, handle_save_get);
    s_server.on("/save", HTTP_POST, handle_save_post);
    s_server.on("/reboot", HTTP_GET, handle_reboot);
    s_server.on("/factory-reset", HTTP_GET, handle_factory_reset);
    s_server.onNotFound(handle_not_found);
    s_server.begin();
    s_server_started = true;
}

void webconfig_poll(void)
{
    if (!s_server_started) return;
    s_server.poll_limited();
}

const WebUiServerStats *webconfig_get_stats(void)
{
    return s_server.stats();
}
