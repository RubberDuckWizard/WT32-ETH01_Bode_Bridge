#include <Arduino.h>
#include <WiFi.h>
#include <stdio.h>
#include <string.h>

#include "esp_config.h"
#include "esp_persist.h"
#include "esp_runtime_net.h"
#include "esp_scope_vnc_proxy.h"

namespace {

struct ProxyConnection {
    bool active;
    WiFiClient client;
    WiFiClient upstream;
    IPAddress client_ip;
    uint16_t client_port;
    uint32_t opened_ms;
    uint32_t last_activity_ms;
    uint32_t client_to_scope_bytes;
    uint32_t scope_to_client_bytes;
    size_t request_head_len;
    bool request_head_done;
    char request_head[SCOPE_VNC_PROXY_SNIFF_BYTES + 1U];
};

static WiFiServer s_proxy_server(SCOPE_VNC_PROXY_LISTEN_PORT);
static ScopeVncProxyStats s_stats;
static ProxyConnection s_connections[SCOPE_VNC_PROXY_MAX_CONNECTIONS];
static bool s_initialized = false;
static bool s_listening = false;
static uint8_t s_poll_cursor = 0u;

static void poll_connection(ProxyConnection *conn);

static void update_runtime_flags()
{
    uint8_t active = 0u;

    s_stats.supported = ENABLE_ETH_RUNTIME != 0;
    s_stats.enabled = g_config.scope_http_proxy_enable != 0u;
    s_stats.listening = s_listening;
    s_stats.listen_port = SCOPE_VNC_PROXY_LISTEN_PORT;
    s_stats.target_port = SCOPE_VNC_PROXY_TARGET_PORT;
    s_stats.max_connections = SCOPE_VNC_PROXY_MAX_CONNECTIONS;
    for (size_t i = 0; i < SCOPE_VNC_PROXY_MAX_CONNECTIONS; ++i) {
        if (s_connections[i].active) {
            ++active;
        }
    }
    s_stats.active_connections = active;
}

static void set_last_error(const char *text)
{
    if (text == NULL) {
        text = "unknown";
    }
    strncpy(s_stats.last_error, text, sizeof(s_stats.last_error) - 1u);
    s_stats.last_error[sizeof(s_stats.last_error) - 1u] = '\0';
}

static void clear_connection(ProxyConnection *conn)
{
    if (conn == NULL) {
        return;
    }
    if (conn->client) {
        conn->client.stop();
    }
    if (conn->upstream) {
        conn->upstream.stop();
    }
    conn->active = false;
    conn->client = WiFiClient();
    conn->upstream = WiFiClient();
    conn->client_ip = IPAddress();
    conn->client_port = 0u;
    conn->opened_ms = 0u;
    conn->last_activity_ms = 0u;
    conn->client_to_scope_bytes = 0u;
    conn->scope_to_client_bytes = 0u;
    conn->request_head_len = 0u;
    conn->request_head_done = false;
    conn->request_head[0] = '\0';
}

static size_t bounded_chunk_len(int available_len)
{
    return available_len > (int)SCOPE_VNC_PROXY_BUFFER_SIZE
        ? (size_t)SCOPE_VNC_PROXY_BUFFER_SIZE
        : (size_t)available_len;
}

static IPAddress scope_target_ip()
{
    return IPAddress(g_config.scope_ip[0], g_config.scope_ip[1], g_config.scope_ip[2], g_config.scope_ip[3]);
}

static uint32_t proxy_connect_timeout_ms()
{
    uint32_t timeout_ms = g_config.scope_connect_timeout_ms;

    if (timeout_ms < 250u) {
        timeout_ms = 250u;
    }
    if (timeout_ms > 1500u) {
        timeout_ms = 1500u;
    }
    return timeout_ms;
}

static bool should_listen()
{
    return ENABLE_ETH_RUNTIME != 0
        && g_config.scope_http_proxy_enable != 0u
        && runtime_net_is_ready_for_web();
}

static bool write_all(WiFiClient &dst, const uint8_t *data, size_t len)
{
    size_t offset = 0u;

    while (offset < len) {
        size_t written = dst.write(data + offset, len - offset);
        if (written == 0u) {
            return false;
        }
        offset += written;
        if (offset < len) {
            yield();
        }
    }
    return true;
}

static void inspect_request_head(ProxyConnection *conn)
{
    if (conn == NULL || conn->request_head_done) {
        return;
    }

    if (!s_stats.detected_websockify_path
            && strstr(conn->request_head, "GET /websockify") != NULL) {
        s_stats.detected_websockify_path = true;
        Serial.printf("[proxy_vnc] request_path client=%s:%u path=/websockify\r\n",
            conn->client_ip.toString().c_str(),
            (unsigned)conn->client_port);
    }
    if (!s_stats.detected_websocket_upgrade
            && strstr(conn->request_head, "Upgrade: websocket") != NULL) {
        s_stats.detected_websocket_upgrade = true;
        Serial.printf("[proxy_vnc] websocket_upgrade client=%s:%u\r\n",
            conn->client_ip.toString().c_str(),
            (unsigned)conn->client_port);
    }
    conn->request_head_done = true;
}

static void capture_request_head(ProxyConnection *conn, const uint8_t *data, size_t len)
{
    size_t remaining;
    size_t copy_len;

    if (conn == NULL || conn->request_head_done || len == 0u) {
        return;
    }

    remaining = SCOPE_VNC_PROXY_SNIFF_BYTES - conn->request_head_len;
    copy_len = len < remaining ? len : remaining;
    if (copy_len > 0u) {
        memcpy(conn->request_head + conn->request_head_len, data, copy_len);
        conn->request_head_len += copy_len;
        conn->request_head[conn->request_head_len] = '\0';
    }
    if (strstr(conn->request_head, "\r\n\r\n") != NULL
            || conn->request_head_len >= SCOPE_VNC_PROXY_SNIFF_BYTES) {
        inspect_request_head(conn);
    }
}

static void close_connection(ProxyConnection *conn, const char *reason, bool dropped)
{
    int slot_index;
    uint32_t age_ms;

    if (conn == NULL || !conn->active) {
        return;
    }

    slot_index = (int)(conn - s_connections);
    age_ms = millis() - conn->opened_ms;
    Serial.printf("[proxy_vnc] close slot=%d client=%s:%u reason=%s c2s=%lu s2c=%lu age_ms=%lu\r\n",
        slot_index,
        conn->client_ip.toString().c_str(),
        (unsigned)conn->client_port,
        reason != NULL ? reason : "unknown",
        (unsigned long)conn->client_to_scope_bytes,
        (unsigned long)conn->scope_to_client_bytes,
        (unsigned long)age_ms);

    if (dropped) {
        ++s_stats.dropped_connections;
    } else {
        ++s_stats.completed_connections;
    }
    s_stats.total_client_to_scope_bytes += conn->client_to_scope_bytes;
    s_stats.total_scope_to_client_bytes += conn->scope_to_client_bytes;
    clear_connection(conn);
    update_runtime_flags();
}

static void stop_all_connections(const char *reason)
{
    for (size_t i = 0; i < SCOPE_VNC_PROXY_MAX_CONNECTIONS; ++i) {
        if (s_connections[i].active) {
            close_connection(&s_connections[i], reason, false);
        }
    }
}

static void stop_listening(const char *reason)
{
    if (!s_listening) {
        update_runtime_flags();
        return;
    }

    stop_all_connections(reason != NULL ? reason : "stop");
    s_proxy_server.end();
    s_listening = false;
    update_runtime_flags();
    Serial.printf("[proxy_vnc] stop reason=%s\r\n", reason != NULL ? reason : "stop");
}

static void start_listening()
{
    if (s_listening || !should_listen()) {
        update_runtime_flags();
        return;
    }

    s_proxy_server.begin();
    s_proxy_server.setNoDelay(true);
    s_listening = true;
    set_last_error("none");
    update_runtime_flags();
    Serial.printf("[proxy_vnc] listen port=%u target=%u.%u.%u.%u:%u\r\n",
        (unsigned)SCOPE_VNC_PROXY_LISTEN_PORT,
        g_config.scope_ip[0], g_config.scope_ip[1], g_config.scope_ip[2], g_config.scope_ip[3],
        (unsigned)SCOPE_VNC_PROXY_TARGET_PORT);
}

static int find_free_slot()
{
    for (size_t i = 0; i < SCOPE_VNC_PROXY_MAX_CONNECTIONS; ++i) {
        if (!s_connections[i].active) {
            return (int)i;
        }
    }
    return -1;
}

static bool open_connection(WiFiClient &incoming)
{
    ProxyConnection *conn;
    WiFiClient upstream;
    int slot_index;

    slot_index = find_free_slot();
    if (slot_index < 0) {
        set_last_error("slot_exhausted");
        ++s_stats.dropped_connections;
        Serial.printf("[proxy_vnc] reject client=%s:%u reason=slot_exhausted\r\n",
            incoming.remoteIP().toString().c_str(),
            (unsigned)incoming.remotePort());
        incoming.stop();
        update_runtime_flags();
        return false;
    }

    if (!upstream.connect(scope_target_ip(), SCOPE_VNC_PROXY_TARGET_PORT, proxy_connect_timeout_ms())) {
        set_last_error("scope_connect_failed");
        ++s_stats.failed_connects;
        Serial.printf("[proxy_vnc] connect_fail client=%s:%u target=%u.%u.%u.%u:%u\r\n",
            incoming.remoteIP().toString().c_str(),
            (unsigned)incoming.remotePort(),
            g_config.scope_ip[0], g_config.scope_ip[1], g_config.scope_ip[2], g_config.scope_ip[3],
            (unsigned)SCOPE_VNC_PROXY_TARGET_PORT);
        incoming.stop();
        update_runtime_flags();
        return false;
    }

    incoming.setNoDelay(true);
    upstream.setNoDelay(true);

    conn = &s_connections[slot_index];
    clear_connection(conn);
    conn->active = true;
    conn->client = incoming;
    conn->upstream = upstream;
    conn->client_ip = incoming.remoteIP();
    conn->client_port = incoming.remotePort();
    conn->opened_ms = millis();
    conn->last_activity_ms = conn->opened_ms;
    ++s_stats.accepted_connections;
    set_last_error("none");
    update_runtime_flags();
    Serial.printf("[proxy_vnc] accept slot=%d client=%s:%u target=%u.%u.%u.%u:%u\r\n",
        slot_index,
        conn->client_ip.toString().c_str(),
        (unsigned)conn->client_port,
        g_config.scope_ip[0], g_config.scope_ip[1], g_config.scope_ip[2], g_config.scope_ip[3],
        (unsigned)SCOPE_VNC_PROXY_TARGET_PORT);
    return true;
}

static void accept_pending_clients()
{
    WiFiClient incoming;
    uint8_t accept_budget = SCOPE_VNC_PROXY_MAX_CONNECTIONS;

    if (!s_listening) {
        return;
    }

    while (accept_budget-- > 0u) {
        incoming = s_proxy_server.available();
        if (!incoming) {
            break;
        }
        (void)open_connection(incoming);
    }
}

static bool forward_stream(WiFiClient &src, WiFiClient &dst, ProxyConnection *conn,
    bool sniff_client_head, uint32_t *counter)
{
    uint8_t buf[SCOPE_VNC_PROXY_BUFFER_SIZE];

    if (src.available() <= 0) {
        return true;
    }

    int available_len = src.available();
    size_t chunk_len = bounded_chunk_len(available_len);
    int read_len = src.read(buf, chunk_len);
    if (read_len <= 0) {
        return true;
    }
    if (sniff_client_head) {
        capture_request_head(conn, buf, (size_t)read_len);
    }
    if (!write_all(dst, buf, (size_t)read_len)) {
        return false;
    }
    *counter += (uint32_t)read_len;
    conn->last_activity_ms = millis();
    return true;
}

static void poll_connections_round_robin()
{
    for (size_t step = 0; step < SCOPE_VNC_PROXY_MAX_CONNECTIONS; ++step) {
        size_t index = (size_t)((s_poll_cursor + step) % SCOPE_VNC_PROXY_MAX_CONNECTIONS);

        if (s_connections[index].active) {
            poll_connection(&s_connections[index]);
        }
    }
    s_poll_cursor = (uint8_t)((s_poll_cursor + 1u) % SCOPE_VNC_PROXY_MAX_CONNECTIONS);
}

static void poll_connection(ProxyConnection *conn)
{
    bool client_open;
    bool upstream_open;

    if (conn == NULL || !conn->active) {
        return;
    }

    if (conn->client.available() > 0
            && !forward_stream(conn->client, conn->upstream, conn, true, &conn->client_to_scope_bytes)) {
        set_last_error("scope_write_failed");
        close_connection(conn, "scope_write_failed", true);
        return;
    }
    if (conn->upstream.available() > 0
            && !forward_stream(conn->upstream, conn->client, conn, false, &conn->scope_to_client_bytes)) {
        set_last_error("client_write_failed");
        close_connection(conn, "client_write_failed", true);
        return;
    }

    client_open = conn->client.connected() || conn->client.available() > 0;
    upstream_open = conn->upstream.connected() || conn->upstream.available() > 0;

    if (!client_open) {
        close_connection(conn, "client_closed", false);
        return;
    }
    if (!upstream_open) {
        close_connection(conn, "scope_closed", false);
        return;
    }
    if ((millis() - conn->last_activity_ms) > SCOPE_VNC_PROXY_IDLE_TIMEOUT_MS) {
        set_last_error("idle_timeout");
        close_connection(conn, "idle_timeout", true);
        return;
    }
}

}  // namespace

void scope_vnc_proxy_begin()
{
    if (s_initialized) {
        update_runtime_flags();
        return;
    }

    memset(&s_stats, 0, sizeof(s_stats));
    set_last_error("none");
    for (size_t i = 0; i < SCOPE_VNC_PROXY_MAX_CONNECTIONS; ++i) {
        clear_connection(&s_connections[i]);
    }
    s_initialized = true;
    s_listening = false;
    update_runtime_flags();
}

void scope_vnc_proxy_poll()
{
    if (!s_initialized) {
        scope_vnc_proxy_begin();
    }

    if (!should_listen()) {
        stop_listening(g_config.scope_http_proxy_enable != 0u ? "web_offline" : "disabled");
        return;
    }

    start_listening();
    poll_connections_round_robin();
    accept_pending_clients();
    update_runtime_flags();
}

bool scope_vnc_proxy_is_supported()
{
    return scope_vnc_proxy_get_stats()->supported;
}

bool scope_vnc_proxy_is_enabled()
{
    return scope_vnc_proxy_get_stats()->enabled;
}

bool scope_vnc_proxy_is_listening()
{
    return scope_vnc_proxy_get_stats()->listening;
}

uint16_t scope_vnc_proxy_listen_port()
{
    return scope_vnc_proxy_get_stats()->listen_port;
}

const ScopeVncProxyStats *scope_vnc_proxy_get_stats()
{
    if (!s_initialized) {
        scope_vnc_proxy_begin();
    }
    return &s_stats;
}