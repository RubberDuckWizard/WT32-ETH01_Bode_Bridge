#include <Arduino.h>
#include <WiFi.h>
#include <stdio.h>
#include <string.h>

#include "esp_config.h"
#include "esp_persist.h"
#include "esp_runtime_net.h"
#include "esp_scope_http_proxy.h"

namespace {

struct ProxyConnection {
    bool active;
    bool waiting_for_scope;
    WiFiClient client;
    WiFiClient upstream;
    IPAddress client_ip;
    uint16_t client_port;
    uint32_t opened_ms;
    uint32_t last_activity_ms;
    uint32_t connect_deadline_ms;
    uint32_t client_to_scope_bytes;
    uint32_t scope_to_client_bytes;
    size_t response_head_len;
    bool response_head_done;
    char response_head[SCOPE_HTTP_PROXY_SNIFF_BYTES + 1U];
};

static WiFiServer s_proxy_server(SCOPE_HTTP_PROXY_LISTEN_PORT);
static ScopeHttpProxyStats s_stats;
static ProxyConnection s_connections[SCOPE_HTTP_PROXY_MAX_CONNECTIONS];
static bool s_initialized = false;
static bool s_listening = false;

static uint8_t proxy_max_connections()
{
    uint8_t value = g_config.max_scope_http_proxy_clients;

    if (value < MIN_WEB_SERVICE_CLIENTS) {
        value = MIN_WEB_SERVICE_CLIENTS;
    }
    if (value > SCOPE_HTTP_PROXY_MAX_CONNECTIONS) {
        value = SCOPE_HTTP_PROXY_MAX_CONNECTIONS;
    }
    return value;
}

static void update_runtime_flags()
{
    uint8_t active = 0;

    s_stats.supported = ENABLE_ETH_RUNTIME != 0;
    s_stats.enabled = g_config.scope_http_proxy_enable != 0u;
    s_stats.listening = s_listening;
    s_stats.listen_port = SCOPE_HTTP_PROXY_LISTEN_PORT;
    s_stats.max_connections = proxy_max_connections();
    for (size_t i = 0; i < SCOPE_HTTP_PROXY_MAX_CONNECTIONS; ++i) {
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
    conn->waiting_for_scope = false;
    conn->client = WiFiClient();
    conn->upstream = WiFiClient();
    conn->client_ip = IPAddress();
    conn->client_port = 0u;
    conn->opened_ms = 0u;
    conn->last_activity_ms = 0u;
    conn->connect_deadline_ms = 0u;
    conn->client_to_scope_bytes = 0u;
    conn->scope_to_client_bytes = 0u;
    conn->response_head_len = 0u;
    conn->response_head_done = false;
    conn->response_head[0] = '\0';
}

static uint8_t active_upstream_count()
{
    uint8_t active = 0;

    for (size_t i = 0; i < SCOPE_HTTP_PROXY_MAX_CONNECTIONS; ++i) {
        if (s_connections[i].active && !s_connections[i].waiting_for_scope) {
            ++active;
        }
    }
    return active;
}

static void reject_with_503(WiFiClient &incoming, const char *reason)
{
    incoming.setNoDelay(true);
    (void)incoming.print(
        "HTTP/1.1 503 Service Unavailable\r\n"
        "Connection: close\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 19\r\n"
        "\r\n"
        "Service Unavailable");
    incoming.stop();
    set_last_error(reason != NULL ? reason : "limit_reached");
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

static void format_scope_target(char *dst, size_t dst_len)
{
    snprintf(dst, dst_len, "%u.%u.%u.%u:%u",
        g_config.scope_ip[0], g_config.scope_ip[1], g_config.scope_ip[2], g_config.scope_ip[3],
        (unsigned)SCOPE_HTTP_PROXY_TARGET_PORT);
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

static void inspect_response_header(ProxyConnection *conn)
{
    char scope_host[16];

    if (conn == NULL || conn->response_head_done) {
        return;
    }

    snprintf(scope_host, sizeof(scope_host), "%u.%u.%u.%u",
        g_config.scope_ip[0], g_config.scope_ip[1], g_config.scope_ip[2], g_config.scope_ip[3]);

    if (!s_stats.detected_absolute_location
            && strstr(conn->response_head, "Location: http://") != NULL
            && strstr(conn->response_head, scope_host) != NULL) {
        s_stats.detected_absolute_location = true;
        Serial.printf("[proxy] compatibility_hint absolute_location target=%s\r\n", scope_host);
    }
    if (!s_stats.detected_absolute_scope_url
            && strstr(conn->response_head, "http://") != NULL
            && strstr(conn->response_head, scope_host) != NULL) {
        s_stats.detected_absolute_scope_url = true;
        Serial.printf("[proxy] compatibility_hint absolute_scope_url target=%s\r\n", scope_host);
    }
    conn->response_head_done = true;
}

static void capture_response_head(ProxyConnection *conn, const uint8_t *data, size_t len)
{
    size_t remaining;
    size_t copy_len;

    if (conn == NULL || conn->response_head_done || len == 0u) {
        return;
    }

    remaining = SCOPE_HTTP_PROXY_SNIFF_BYTES - conn->response_head_len;
    copy_len = len < remaining ? len : remaining;
    if (copy_len > 0u) {
        memcpy(conn->response_head + conn->response_head_len, data, copy_len);
        conn->response_head_len += copy_len;
        conn->response_head[conn->response_head_len] = '\0';
    }
    if (strstr(conn->response_head, "\r\n\r\n") != NULL
            || conn->response_head_len >= SCOPE_HTTP_PROXY_SNIFF_BYTES) {
        inspect_response_header(conn);
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
    Serial.printf("[proxy] close slot=%d client=%s:%u reason=%s c2s=%lu s2c=%lu age_ms=%lu\r\n",
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
    for (size_t i = 0; i < SCOPE_HTTP_PROXY_MAX_CONNECTIONS; ++i) {
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
    Serial.printf("[proxy] stop reason=%s\r\n", reason != NULL ? reason : "stop");
}

static void start_listening()
{
    char target[24];

    if (s_listening || !should_listen()) {
        update_runtime_flags();
        return;
    }

    s_proxy_server.begin();
    s_proxy_server.setNoDelay(true);
    s_listening = true;
    set_last_error("none");
    update_runtime_flags();

    format_scope_target(target, sizeof(target));
    Serial.printf("[proxy] listen port=%u target=%s\r\n",
        (unsigned)SCOPE_HTTP_PROXY_LISTEN_PORT,
        target);
}

static int find_free_slot()
{
    for (size_t i = 0; i < SCOPE_HTTP_PROXY_MAX_CONNECTIONS; ++i) {
        if (!s_connections[i].active) {
            return (int)i;
        }
    }
    return -1;
}

static bool connect_upstream(ProxyConnection *conn)
{
    WiFiClient upstream;
    char target[24];

    if (conn == NULL || !conn->active || !conn->waiting_for_scope) {
        return false;
    }
    if (active_upstream_count() >= proxy_max_connections()) {
        return false;
    }

    if (!upstream.connect(scope_target_ip(), SCOPE_HTTP_PROXY_TARGET_PORT, proxy_connect_timeout_ms())) {
        return false;
    }

    upstream.setNoDelay(true);
    conn->upstream = upstream;
    conn->waiting_for_scope = false;
    conn->last_activity_ms = millis();
    set_last_error("none");
    format_scope_target(target, sizeof(target));
    Serial.printf("[proxy] upstream_ready client=%s:%u target=%s\r\n",
        conn->client_ip.toString().c_str(),
        (unsigned)conn->client_port,
        target);
    return true;
}

static bool open_connection(WiFiClient &incoming)
{
    ProxyConnection *conn;
    int slot_index;
    char target[24];
    uint8_t active_before = s_stats.active_connections;
    uint8_t max_connections = proxy_max_connections();

    slot_index = find_free_slot();
    if (slot_index < 0 || active_before >= max_connections) {
        ++s_stats.dropped_connections;
        Serial.printf("[proxy] reject client=%s:%u reason=limit_reached active=%u max=%u\r\n",
            incoming.remoteIP().toString().c_str(),
            (unsigned)incoming.remotePort(),
            (unsigned)active_before,
            (unsigned)max_connections);
        reject_with_503(incoming, "limit_reached");
        update_runtime_flags();
        return false;
    }

    incoming.setNoDelay(true);

    conn = &s_connections[slot_index];
    clear_connection(conn);
    conn->active = true;
    conn->waiting_for_scope = true;
    conn->client = incoming;
    conn->client_ip = incoming.remoteIP();
    conn->client_port = incoming.remotePort();
    conn->opened_ms = millis();
    conn->last_activity_ms = conn->opened_ms;
    conn->connect_deadline_ms = conn->opened_ms + SCOPE_HTTP_PROXY_IDLE_TIMEOUT_MS;
    ++s_stats.accepted_connections;
    update_runtime_flags();

    format_scope_target(target, sizeof(target));
    Serial.printf("[proxy] accept slot=%d client=%s:%u target=%s mode=%s active=%u max=%u\r\n",
        slot_index,
        conn->client_ip.toString().c_str(),
        (unsigned)conn->client_port,
        target,
        active_upstream_count() < proxy_max_connections() ? "direct" : "queued",
        (unsigned)s_stats.active_connections,
        (unsigned)proxy_max_connections());
    if (!connect_upstream(conn)) {
        set_last_error("scope_upstream_queued");
    }
    return true;
}

static void accept_pending_clients()
{
    WiFiClient incoming;

    if (!s_listening) {
        return;
    }

    for (;;) {
        incoming = s_proxy_server.available();
        if (!incoming) {
            break;
        }
        (void)open_connection(incoming);
    }
}

static bool forward_stream(WiFiClient &src, WiFiClient &dst, ProxyConnection *conn,
    bool capture_head, uint32_t *counter)
{
    uint8_t buf[SCOPE_HTTP_PROXY_BUFFER_SIZE];

    while (src.available() > 0) {
        int available_len = src.available();
        size_t chunk_len = available_len > (int)sizeof(buf) ? sizeof(buf) : (size_t)available_len;
        int read_len = src.read(buf, chunk_len);
        if (read_len <= 0) {
            break;
        }
        if (capture_head) {
            capture_response_head(conn, buf, (size_t)read_len);
        }
        if (!write_all(dst, buf, (size_t)read_len)) {
            return false;
        }
        *counter += (uint32_t)read_len;
        conn->last_activity_ms = millis();
        yield();
    }
    return true;
}

static void poll_connection(ProxyConnection *conn)
{
    bool client_open;
    bool upstream_open;

    if (conn == NULL || !conn->active) {
        return;
    }

    if (conn->waiting_for_scope) {
        if (!(conn->client.connected() || conn->client.available() > 0)) {
            close_connection(conn, "client_closed", false);
            return;
        }
        if (millis() > conn->connect_deadline_ms) {
            ++s_stats.failed_connects;
            set_last_error("scope_connect_timeout");
            close_connection(conn, "scope_connect_timeout", true);
            return;
        }
        if (!connect_upstream(conn)) {
            return;
        }
    }

    if (conn->client.available() > 0
            && !forward_stream(conn->client, conn->upstream, conn, false, &conn->client_to_scope_bytes)) {
        set_last_error("scope_write_failed");
        close_connection(conn, "scope_write_failed", true);
        return;
    }
    if (conn->upstream.available() > 0
            && !forward_stream(conn->upstream, conn->client, conn, true, &conn->scope_to_client_bytes)) {
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
    if ((millis() - conn->last_activity_ms) > SCOPE_HTTP_PROXY_IDLE_TIMEOUT_MS) {
        set_last_error("idle_timeout");
        close_connection(conn, "idle_timeout", true);
        return;
    }
}

}  // namespace

void scope_http_proxy_begin()
{
    if (s_initialized) {
        update_runtime_flags();
        return;
    }

    memset(&s_stats, 0, sizeof(s_stats));
    set_last_error("none");
    for (size_t i = 0; i < SCOPE_HTTP_PROXY_MAX_CONNECTIONS; ++i) {
        clear_connection(&s_connections[i]);
    }
    s_initialized = true;
    s_listening = false;
    update_runtime_flags();
}

void scope_http_proxy_poll()
{
    if (!s_initialized) {
        scope_http_proxy_begin();
    }

    if (!should_listen()) {
        stop_listening(g_config.scope_http_proxy_enable != 0u ? "web_offline" : "disabled");
        return;
    }

    start_listening();
    for (size_t i = 0; i < SCOPE_HTTP_PROXY_MAX_CONNECTIONS; ++i) {
        if (s_connections[i].active) {
            poll_connection(&s_connections[i]);
        }
    }
    accept_pending_clients();
    update_runtime_flags();
}

bool scope_http_proxy_is_supported()
{
    return scope_http_proxy_get_stats()->supported;
}

bool scope_http_proxy_is_enabled()
{
    return scope_http_proxy_get_stats()->enabled;
}

bool scope_http_proxy_is_listening()
{
    return scope_http_proxy_get_stats()->listening;
}

uint16_t scope_http_proxy_listen_port()
{
    return scope_http_proxy_get_stats()->listen_port;
}

const ScopeHttpProxyStats *scope_http_proxy_get_stats()
{
    if (!s_initialized) {
        scope_http_proxy_begin();
    }
    return &s_stats;
}
