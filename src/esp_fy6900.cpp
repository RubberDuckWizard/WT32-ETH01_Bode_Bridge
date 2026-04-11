#include <Arduino.h>
#include <string.h>
#include <stdio.h>

#include "esp_fy6900.h"
#include "esp_persist.h"
#include "esp_config.h"
#include "esp_diag_console.h"
#include "esp_parser.h"

AwgState g_awg;
static AwgState s_awg_sent;
static bool s_awg_enabled = ENABLE_AWG_AUTOSTREAM ? true : false;
static uint32_t s_awg_baud = AWG_BAUD_RATE;
static uint16_t s_awg_timeout_ms = DEF_AWG_SERIAL_TIMEOUT_MS;
static uint32_t s_awg_uart_config = SERIAL_8N2;
static char s_awg_serial_mode[5] = DEF_AWG_SERIAL_MODE;
static bool s_awg_sent_valid = false;
static bool s_awg_serial_started = false;
static HardwareSerial &s_awg_serial = Serial2;
static const char *s_awg_status = ENABLE_AWG_AUTOSTREAM ? "pending" : "disabled";
static uint32_t s_awg_boot_deadline_ms = 0;
static char s_awg_last_manual_test[160] = "not requested";
static FyDiagSnapshot s_fy_diag;

enum {
    FY_PRESENT_UNKNOWN = 0,
    FY_PRESENT_PRESENT = 1,
    FY_PRESENT_ABSENT  = 2
};

#if FW_DEFER_FY_INIT
static const uint16_t FY_BOOT_INIT_BUDGET_MS = 180;
#endif

static void set_awg_status(const char *status)
{
    s_awg_status = (status != NULL && status[0] != '\0') ? status : "timeout";
    strncpy(s_fy_diag.current_status, s_awg_status, sizeof(s_fy_diag.current_status) - 1);
    s_fy_diag.current_status[sizeof(s_fy_diag.current_status) - 1] = '\0';
}

static void set_manual_test_summary(const char *text)
{
    if (text == NULL) text = "not requested";
    strncpy(s_awg_last_manual_test, text, sizeof(s_awg_last_manual_test) - 1);
    s_awg_last_manual_test[sizeof(s_awg_last_manual_test) - 1] = '\0';
}

static void diag_copy_text(char *dst, size_t dst_len, const char *src)
{
    if (dst_len == 0) return;
    if (src == NULL) src = "";
    strncpy(dst, src, dst_len - 1);
    dst[dst_len - 1] = '\0';
}

static const char *diag_presence_label(uint8_t state)
{
    switch (state) {
    case FY_PRESENT_PRESENT: return "present";
    case FY_PRESENT_ABSENT:  return "absent";
    default:                 return "unknown";
    }
}

static void diag_set_presence(uint8_t state)
{
    s_fy_diag.fy_present_state = state;
}

static void diag_mark_operation(const char *name)
{
    diag_copy_text(s_fy_diag.last_operation, sizeof(s_fy_diag.last_operation), name);
    s_fy_diag.last_operation_ms = millis();
    s_fy_diag.operation_count++;
}

static void diag_set_error(const char *text)
{
    diag_copy_text(s_fy_diag.last_error, sizeof(s_fy_diag.last_error), text);
}

static void diag_set_response_kind(const char *text)
{
    diag_copy_text(s_fy_diag.last_response_kind, sizeof(s_fy_diag.last_response_kind), text);
}

static void diag_set_umo_parser_status(const char *text)
{
    diag_copy_text(s_fy_diag.last_umo_parser_status,
        sizeof(s_fy_diag.last_umo_parser_status), text);
}

static void diag_set_sequence_result(const char *text)
{
    diag_copy_text(s_fy_diag.last_sequence_result,
        sizeof(s_fy_diag.last_sequence_result), text);
}

static void diag_set_parser_readback(const char *text)
{
    diag_copy_text(s_fy_diag.last_parser_readback,
        sizeof(s_fy_diag.last_parser_readback), text);
}

static void diag_format_hex(const uint8_t *src, size_t len, char *dst, size_t dst_len)
{
    size_t offset = 0;

    if (dst_len == 0) return;
    dst[0] = '\0';
    if (src == NULL || len == 0) return;

    for (size_t i = 0; i < len && offset + 3 < dst_len; ++i) {
        int n = snprintf(dst + offset, dst_len - offset, "%02X", src[i]);
        if (n <= 0 || (size_t)n >= dst_len - offset) break;
        offset += (size_t)n;
        if (i + 1u < len && offset + 1u < dst_len) {
            dst[offset++] = ' ';
            dst[offset] = '\0';
        }
    }
}

static void diag_format_ascii(const uint8_t *src, size_t len, char *dst, size_t dst_len)
{
    size_t offset = 0;

    if (dst_len == 0) return;
    dst[0] = '\0';
    if (src == NULL || len == 0) return;

    for (size_t i = 0; i < len && offset + 2u < dst_len; ++i) {
        uint8_t ch = src[i];
        if (ch >= 32u && ch <= 126u) {
            dst[offset++] = (char)ch;
        } else if (ch == '\r' && offset + 2u < dst_len) {
            dst[offset++] = '\\';
            dst[offset++] = 'r';
        } else if (ch == '\n' && offset + 2u < dst_len) {
            dst[offset++] = '\\';
            dst[offset++] = 'n';
        } else if (ch == '\t' && offset + 2u < dst_len) {
            dst[offset++] = '\\';
            dst[offset++] = 't';
        } else {
            dst[offset++] = '.';
        }
    }
    dst[offset] = '\0';
}

static bool escaped_text_has_lf_terminator(const char *text)
{
    size_t len;

    if (text == NULL) return false;
    len = strlen(text);
    return len >= 2u && text[len - 2u] == '\\' && text[len - 1u] == 'n';
}

static bool parse_umo_payload_from_escaped(const char *escaped, char *parsed,
    size_t parsed_len, const char **status_out)
{
    size_t out = 0u;
    bool saw_visible = false;

    if (parsed != NULL && parsed_len > 0u) {
        parsed[0] = '\0';
    }
    if (status_out != NULL) {
        *status_out = "empty";
    }
    if (escaped == NULL || escaped[0] == '\0' || parsed == NULL || parsed_len == 0u) {
        return false;
    }

    for (size_t i = 0; escaped[i] != '\0'; ++i) {
        if (escaped[i] == '\\' && escaped[i + 1u] != '\0') {
            char next = escaped[i + 1u];
            if (next == 'n' || next == 'r') {
                if (saw_visible) {
                    break;
                }
                ++i;
                continue;
            }
            if (next == 't') {
                if (saw_visible && out + 1u < parsed_len) {
                    parsed[out++] = ' ';
                }
                ++i;
                continue;
            }
        }

        if (!saw_visible && escaped[i] == ' ') {
            continue;
        }
        if ((unsigned char)escaped[i] < 32u || (unsigned char)escaped[i] > 126u) {
            if (status_out != NULL) {
                *status_out = "invalid chars";
            }
            return false;
        }
        saw_visible = true;
        if (out + 1u < parsed_len) {
            parsed[out++] = escaped[i];
        }
    }

    while (out > 0u && parsed[out - 1u] == ' ') {
        --out;
    }
    parsed[out] = '\0';

    if (!saw_visible || out == 0u) {
        if (status_out != NULL) {
            *status_out = "lf-only ack";
        }
        return false;
    }

    if (status_out != NULL) {
        *status_out = escaped_text_has_lf_terminator(escaped)
            ? "payload valid"
            : "payload no lf";
    }
    return true;
}

static void diag_record_tx(const uint8_t *src, size_t len)
{
    diag_format_ascii(src, len, s_fy_diag.last_tx_raw, sizeof(s_fy_diag.last_tx_raw));
    diag_format_hex(src, len, s_fy_diag.last_tx_hex, sizeof(s_fy_diag.last_tx_hex));
    s_fy_diag.tx_total_bytes += (uint32_t)len;
}

static void diag_record_rx(const uint8_t *src, size_t len, bool saw_invalid)
{
    diag_format_ascii(src, len, s_fy_diag.last_rx_raw, sizeof(s_fy_diag.last_rx_raw));
    diag_format_ascii(src, len, s_fy_diag.last_rx_ascii, sizeof(s_fy_diag.last_rx_ascii));
    diag_format_hex(src, len, s_fy_diag.last_rx_hex, sizeof(s_fy_diag.last_rx_hex));
    s_fy_diag.rx_total_bytes += (uint32_t)len;
    if (saw_invalid) {
        s_fy_diag.invalid_response_count++;
    }
}

static size_t serial_write_bytes(const uint8_t *src, size_t len)
{
    if (src == NULL || len == 0u) return 0u;
    diag_record_tx(src, len);
    return s_awg_serial.write(src, len);
}

static int diag_capture_raw_response(uint8_t *buf, size_t buf_len, uint16_t timeout_ms,
    bool *timed_out, bool *saw_invalid)
{
    uint32_t start_ms = millis();
    uint32_t last_rx_ms = 0;
    size_t count = 0;

    if (timed_out) *timed_out = false;
    if (saw_invalid) *saw_invalid = false;
    if (buf == NULL || buf_len == 0u) return 0;

    while ((uint32_t)(millis() - start_ms) < timeout_ms) {
        bool had_data = false;
        while (s_awg_serial.available()) {
            int value = s_awg_serial.read();
            if (value < 0) break;
            had_data = true;
            last_rx_ms = millis();
            if (count < buf_len) {
                buf[count++] = (uint8_t)value;
            }
            if (value != '\r' && value != '\n' && value != '\t' && (value < 32 || value > 126)) {
                if (saw_invalid) *saw_invalid = true;
            }
        }
        if (count > 0u && !had_data && (uint32_t)(millis() - last_rx_ms) > 40u) {
            break;
        }
        delay(5);
        yield();
    }

    if (count == 0u && timed_out) {
        *timed_out = true;
    }
    return (int)count;
}

static bool response_has_printable_payload(const uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        uint8_t ch = buf[i];
        if (ch >= 32u && ch <= 126u) {
            return true;
        }
    }
    return false;
}

static bool response_is_lf_only_ack(const uint8_t *buf, size_t len)
{
    if (buf == NULL || len == 0u) return false;
    for (size_t i = 0; i < len; ++i) {
        if (buf[i] != '\n' && buf[i] != '\r') {
            return false;
        }
    }
    return true;
}

static bool boot_budget_expired(void)
{
    return s_awg_boot_deadline_ms != 0
        && (int32_t)(millis() - s_awg_boot_deadline_ms) >= 0;
}

static void copy_serial_mode(char *dst, size_t dst_len, const char *src)
{
    if (dst_len == 0) return;
    if (src == NULL) src = DEF_AWG_SERIAL_MODE;
    strncpy(dst, src, dst_len - 1);
    dst[dst_len - 1] = '\0';
}

static uint32_t serial_mode_to_uart_config(const char *mode)
{
    if (mode == NULL) return SERIAL_8N2;
    if (strcmp(mode, "8N1") == 0) return SERIAL_8N1;
    if (strcmp(mode, "8N2") == 0) return SERIAL_8N2;
    if (strcmp(mode, "8E1") == 0) return SERIAL_8E1;
    if (strcmp(mode, "8E2") == 0) return SERIAL_8E2;
    if (strcmp(mode, "8O1") == 0) return SERIAL_8O1;
    if (strcmp(mode, "8O2") == 0) return SERIAL_8O2;
    if (strcmp(mode, "7E1") == 0) return SERIAL_7E1;
    if (strcmp(mode, "7E2") == 0) return SERIAL_7E2;
    if (strcmp(mode, "7O1") == 0) return SERIAL_7O1;
    if (strcmp(mode, "7O2") == 0) return SERIAL_7O2;
    return SERIAL_8N2;
}

static void load_runtime_serial_config(void)
{
    s_awg_baud = fy_is_supported_baud(g_config.awg_baud) ? g_config.awg_baud : AWG_BAUD_RATE;
    s_awg_timeout_ms = (g_config.awg_serial_timeout_ms >= MIN_AWG_TIMEOUT_MS
        && g_config.awg_serial_timeout_ms <= MAX_AWG_TIMEOUT_MS)
        ? g_config.awg_serial_timeout_ms
        : DEF_AWG_SERIAL_TIMEOUT_MS;

    if (!fy_is_supported_serial_mode(g_config.awg_serial_mode)) {
        copy_serial_mode(s_awg_serial_mode, sizeof(s_awg_serial_mode), DEF_AWG_SERIAL_MODE);
    } else {
        copy_serial_mode(s_awg_serial_mode, sizeof(s_awg_serial_mode), g_config.awg_serial_mode);
    }
    s_awg_uart_config = serial_mode_to_uart_config(s_awg_serial_mode);
    s_fy_diag.baud = s_awg_baud;
    s_fy_diag.timeout_ms = s_awg_timeout_ms;
    s_fy_diag.rx_pin = AWG_UART_RX_PIN;
    s_fy_diag.tx_pin = AWG_UART_TX_PIN;
    diag_copy_text(s_fy_diag.serial_mode, sizeof(s_fy_diag.serial_mode), s_awg_serial_mode);
}

static void restart_awg_serial(void)
{
#if FW_DISABLE_FY_HARDWARE
    s_awg_serial_started = false;
    s_fy_diag.serial_initialized = 0u;
    set_awg_status("disabled");
    return;
#else
    s_awg_serial.flush();
    s_awg_serial.begin(s_awg_baud, s_awg_uart_config, AWG_UART_RX_PIN, AWG_UART_TX_PIN);
    s_awg_serial.setTimeout(s_awg_timeout_ms);
    s_awg_serial_started = true;
    s_fy_diag.serial_initialized = 1u;
#endif
}

static void fy_reset_cache(void)
{
    g_awg.ch1_on = 0;
    g_awg.ch1_wave = AWG_SINE;
    g_awg.ch1_freq_hz = 1000.0;
    g_awg.ch1_ampl_v = 1.0;
    g_awg.ch1_ofst_v = 0.0;
    g_awg.ch1_drive_ampl_v = 2.0;
    g_awg.ch1_drive_ofst_v = 0.0;
    g_awg.ch1_phase_deg = 0.0;
    g_awg.ch1_load = AWG_LOAD_50;

    g_awg.ch2_on = 0;
    g_awg.ch2_wave = AWG_SINE;
    g_awg.ch2_freq_hz = 1000.0;
    g_awg.ch2_ampl_v = 1.0;
    g_awg.ch2_ofst_v = 0.0;
    g_awg.ch2_drive_ampl_v = 2.0;
    g_awg.ch2_drive_ofst_v = 0.0;
    g_awg.ch2_phase_deg = 0.0;
    g_awg.ch2_load = AWG_LOAD_50;
}

static void fy_reset_sent_state(void)
{
    memset(&s_awg_sent, 0, sizeof(s_awg_sent));
    s_awg_sent_valid = false;
}

static void fy_reset_functional_diag_state(void)
{
    s_fy_diag.last_umo_raw[0] = '\0';
    s_fy_diag.last_umo_parsed[0] = '\0';
    diag_set_umo_parser_status("not requested");
    diag_set_sequence_result("not requested");
    diag_set_parser_readback("");
    s_fy_diag.last_functional_level = 0u;
    s_fy_diag.last_functional_success = 0u;
}

static bool same_value(double left, double right, double epsilon)
{
    double delta = left - right;
    if (delta < 0.0) delta = -delta;
    return delta < epsilon;
}

static double fy_load_coeff(AwgLoadMode load)
{
    switch (load) {
    case AWG_LOAD_50:
        return 50.0 / (50.0 + 50.0);
    case AWG_LOAD_75:
        return 75.0 / (75.0 + 50.0);
    case AWG_LOAD_HIZ:
    default:
        return 1.0;
    }
}

static void fy_update_drive_values(uint8_t ch)
{
    double coeff;

    if (ch == 1) {
        coeff = fy_load_coeff(g_awg.ch1_load);
        g_awg.ch1_drive_ampl_v = g_awg.ch1_ampl_v / coeff;
        g_awg.ch1_drive_ofst_v = g_awg.ch1_ofst_v / coeff;
    } else {
        coeff = fy_load_coeff(g_awg.ch2_load);
        g_awg.ch2_drive_ampl_v = g_awg.ch2_ampl_v / coeff;
        g_awg.ch2_drive_ofst_v = g_awg.ch2_ofst_v / coeff;
    }
}

static void awg_trace_tx(const char *buf, size_t len)
{
    diag_tracef("[awg tx] %.*s", (int)len, buf);
}

static bool is_valid_generator_rx_byte(uint8_t value)
{
    return value == '\r' || value == '\n' || value == '\t' || (value >= 32u && value <= 126u);
}

static int read_filtered_serial(char *buf, int buf_max, bool *saw_invalid)
{
    int n = 0;

    if (saw_invalid) *saw_invalid = false;
    if (buf_max <= 0) return 0;

    while (s_awg_serial.available()) {
        uint8_t value = (uint8_t)s_awg_serial.read();
        if (!is_valid_generator_rx_byte(value)) {
            if (saw_invalid) *saw_invalid = true;
            continue;
        }
        if (n < buf_max - 1) {
            buf[n++] = (char)value;
        }
    }

    buf[n] = '\0';
    return n;
}

static inline char ch_letter(uint8_t ch)
{
    return (ch == 1) ? 'M' : 'F';
}

bool fy_is_supported_baud(uint32_t baud)
{
    switch (baud) {
    case 9600u:
    case 19200u:
    case 38400u:
    case 57600u:
    case 115200u:
        return true;
    default:
        return false;
    }
}

bool fy_is_supported_serial_mode(const char *mode)
{
    if (mode == NULL) return false;
    return strcmp(mode, "8N1") == 0
        || strcmp(mode, "8N2") == 0
        || strcmp(mode, "8E1") == 0
        || strcmp(mode, "8E2") == 0
        || strcmp(mode, "8O1") == 0
        || strcmp(mode, "8O2") == 0
        || strcmp(mode, "7E1") == 0
        || strcmp(mode, "7E2") == 0
        || strcmp(mode, "7O1") == 0
        || strcmp(mode, "7O2") == 0;
}

bool fy_is_supported_firmware_family(uint8_t family)
{
    return family == AWG_FW_FAMILY_OLDER || family == AWG_FW_FAMILY_LATER_FY6900;
}

static int fy_format_freq_cmd(char *buf, size_t buf_size, uint8_t ch, double freq_hz)
{
    if (g_config.awg_firmware_family == AWG_FW_FAMILY_OLDER) {
        char digits[15];
        double micro_hz_double;
        uint64_t micro_hz;

        if (freq_hz < 0.0) return 0;

        micro_hz_double = (freq_hz * 1000000.0) + 0.5;
        if (micro_hz_double > 99999999999999.0) return 0;

        micro_hz = (uint64_t)micro_hz_double;
        for (int i = 13; i >= 0; i--) {
            digits[i] = (char)('0' + (micro_hz % 10u));
            micro_hz /= 10u;
        }
        if (micro_hz != 0u) return 0;
        digits[14] = '\0';

        return snprintf(buf, buf_size, "W%cF%s\n", ch_letter(ch), digits);
    }

    return snprintf(buf, buf_size, "W%cF%015.6f\n", ch_letter(ch), freq_hz);
}

static bool wait_ack(void)
{
    uint32_t t0 = millis();
    bool saw_invalid = false;
    char ack_buf[8];

    while (!s_awg_serial.available()) {
        if (boot_budget_expired() || (millis() - t0) > s_awg_timeout_ms) {
            set_awg_status("timeout");
            s_fy_diag.timeout_count++;
            diag_set_error("timeout waiting for FY ack");
            diag_set_presence(FY_PRESENT_ABSENT);
            return false;
        }
        yield();
    }

    int n = read_filtered_serial(ack_buf, sizeof(ack_buf), &saw_invalid);
    if (n > 0) {
        diag_record_rx((const uint8_t *)ack_buf, (size_t)n, saw_invalid);
        set_awg_status("available");
        diag_set_response_kind(response_is_lf_only_ack((const uint8_t *)ack_buf, (size_t)n) ? "lf-only ack" : "payload");
        diag_set_error(saw_invalid ? "valid response with extra invalid bytes" : "none");
        diag_set_presence(FY_PRESENT_PRESENT);
        return true;
    }
    if (saw_invalid) {
        diag_tracef("[awg rx] ignored invalid ttl bytes\r\n");
        s_fy_diag.invalid_response_count++;
        diag_copy_text(s_fy_diag.last_rx_raw, sizeof(s_fy_diag.last_rx_raw), "(invalid bytes only)");
        diag_copy_text(s_fy_diag.last_rx_ascii, sizeof(s_fy_diag.last_rx_ascii), "(invalid bytes only)");
        diag_copy_text(s_fy_diag.last_rx_hex, sizeof(s_fy_diag.last_rx_hex), "(invalid bytes only)");
        diag_set_response_kind("invalid");
        diag_set_error("invalid FY response / noise on RX");
        diag_set_presence(FY_PRESENT_UNKNOWN);
        set_awg_status("invalid");
        return false;
    }
    diag_set_response_kind("no response");
    diag_set_error("no response bytes after TX");
    diag_set_presence(FY_PRESENT_ABSENT);
    return false;
}

static bool send_cmd(const char *buf, size_t len)
{
    if (!s_awg_enabled) {
        set_awg_status("disabled");
        diag_set_response_kind("disabled");
        return false;
    }
    if (boot_budget_expired()) {
        set_awg_status("timeout");
        diag_set_response_kind("timeout");
        return false;
    }
    awg_trace_tx(buf, len);
    (void)serial_write_bytes((const uint8_t *)buf, len);
    return wait_ack();
}

static bool fy_program_channel(uint8_t ch)
{
    char buf[28];
    int n;
    uint8_t on = (ch == 1) ? g_awg.ch1_on : g_awg.ch2_on;
    AwgWaveType wave = (ch == 1) ? g_awg.ch1_wave : g_awg.ch2_wave;
    double freq = (ch == 1) ? g_awg.ch1_freq_hz : g_awg.ch2_freq_hz;
    double drive_ampl = (ch == 1) ? g_awg.ch1_drive_ampl_v : g_awg.ch2_drive_ampl_v;
    double drive_ofst = (ch == 1) ? g_awg.ch1_drive_ofst_v : g_awg.ch2_drive_ofst_v;

    n = snprintf(buf, sizeof(buf), "W%cN%c\n", ch_letter(ch), on ? '1' : '0');
    if (n > 0 && n < (int)sizeof(buf) && !send_cmd(buf, (size_t)n)) return false;

    n = snprintf(buf, sizeof(buf), "W%cW%02u\n", ch_letter(ch), (unsigned)wave);
    if (n > 0 && n < (int)sizeof(buf) && !send_cmd(buf, (size_t)n)) return false;

    n = fy_format_freq_cmd(buf, sizeof(buf), ch, freq);
    if (n > 0 && n < (int)sizeof(buf) && !send_cmd(buf, (size_t)n)) return false;

    n = snprintf(buf, sizeof(buf), "W%cA%.4f\n", ch_letter(ch), drive_ampl);
    if (n > 0 && n < (int)sizeof(buf) && !send_cmd(buf, (size_t)n)) return false;

    n = snprintf(buf, sizeof(buf), "W%cO%.3f\n", ch_letter(ch), drive_ofst);
    if (n > 0 && n < (int)sizeof(buf) && !send_cmd(buf, (size_t)n)) return false;

    return true;
}

static bool fy_resync(void)
{
    char buf[16];
    int n;

    if (!s_awg_enabled) {
        set_awg_status("disabled");
        return false;
    }

    while (s_awg_serial.available()) s_awg_serial.read();

    fy_update_drive_values(1);
    fy_update_drive_values(2);

    if (!fy_program_channel(1)) return false;
    if (!fy_program_channel(2)) return false;

    n = snprintf(buf, sizeof(buf), "WFP%.3f\n", g_awg.ch1_phase_deg);
    if (n > 0 && n < (int)sizeof(buf) && !send_cmd(buf, (size_t)n)) return false;

    s_awg_sent = g_awg;
    s_awg_sent_valid = true;
    set_awg_status("available");
    return true;
}

void fy_init(void)
{
    load_runtime_serial_config();
    restart_awg_serial();
    diag_mark_operation("boot_init");
    diag_set_presence(FY_PRESENT_UNKNOWN);
    diag_set_error("none");
    diag_set_response_kind("not requested");

    delay(10);
    while (s_awg_serial.available()) s_awg_serial.read();

    fy_reset_cache();
    fy_reset_sent_state();
    fy_reset_functional_diag_state();

    if (!s_awg_enabled) {
        set_awg_status("disabled");
        diag_set_response_kind("disabled");
        return;
    }

#if FW_DEFER_FY_INIT
    s_awg_boot_deadline_ms = millis() + FY_BOOT_INIT_BUDGET_MS;
    (void)fy_resync();
    s_awg_boot_deadline_ms = 0;
#else
    (void)fy_resync();
#endif
}

bool fy_is_enabled(void)
{
    return s_awg_enabled;
}

bool fy_serial_initialized(void)
{
    return s_awg_serial_started;
}

const char *fy_status_text(void)
{
    return s_awg_status;
}

const char *fy_last_manual_test_summary(void)
{
    return s_awg_last_manual_test;
}

void fy_set_enabled(bool enabled)
{
    bool was_enabled = s_awg_enabled;
    s_awg_enabled = enabled;
    if (!enabled) {
        set_awg_status("disabled");
        diag_set_response_kind("disabled");
    }
    if (!was_enabled && enabled) {
        fy_reset_sent_state();
        (void)fy_resync();
    }
}

uint32_t fy_get_baud(void)
{
    return s_awg_baud;
}

const char *fy_get_serial_mode(void)
{
    return s_awg_serial_mode;
}

uint16_t fy_get_timeout_ms(void)
{
    return s_awg_timeout_ms;
}

void fy_set_baud(uint32_t baud)
{
    if (!fy_is_supported_baud(baud)) return;
    s_awg_baud = baud;
    restart_awg_serial();
}

void fy_apply_runtime_config(void)
{
#if FW_DISABLE_FY_HARDWARE
    set_awg_status("disabled");
    diag_set_response_kind("disabled");
    return;
#else
    load_runtime_serial_config();
    restart_awg_serial();
    while (s_awg_serial.available()) s_awg_serial.read();
    fy_reset_sent_state();
    fy_reset_functional_diag_state();
    if (s_awg_enabled) {
        (void)fy_resync();
    } else {
        set_awg_status("disabled");
    }
#endif
}

void fy_force_resync(void)
{
    (void)fy_resync();
}

void fy_restore_startup_state(void)
{
    fy_reset_cache();
    fy_reset_sent_state();
    fy_reset_functional_diag_state();
    if (s_awg_enabled) {
        (void)fy_resync();
    } else {
        set_awg_status("disabled");
    }
}

bool fy_diag_uart_init(void)
{
#if FW_DISABLE_FY_HARDWARE
    diag_mark_operation("diag_init");
    diag_set_error("FY hardware disabled in this variant");
    diag_set_response_kind("disabled");
    Serial.printf("[fy diag] init ignored variant=%s hardware=disabled\r\n", FW_VARIANT_NAME);
    return false;
#else
    load_runtime_serial_config();
    restart_awg_serial();
    diag_mark_operation("diag_init");
    diag_set_presence(FY_PRESENT_UNKNOWN);
    diag_set_error("none");
    diag_set_response_kind("not requested");
    set_awg_status("unknown");
    Serial.printf("[fy diag] init serial2=%s rx=%u tx=%u baud=%lu mode=%s timeout=%u variant=%s\r\n",
        s_awg_serial_started ? "yes" : "no",
        (unsigned)AWG_UART_RX_PIN,
        (unsigned)AWG_UART_TX_PIN,
        (unsigned long)s_awg_baud,
        s_awg_serial_mode,
        (unsigned)s_awg_timeout_ms,
        FW_VARIANT_NAME);
    return s_awg_serial_started;
#endif
}

void fy_diag_uart_deinit(void)
{
#if FW_DISABLE_FY_HARDWARE
    diag_mark_operation("diag_deinit");
    diag_set_error("FY hardware disabled in this variant");
    diag_set_response_kind("disabled");
    set_awg_status("disabled");
#else
    s_awg_serial.flush();
    s_awg_serial.end();
    s_awg_serial_started = false;
    s_fy_diag.serial_initialized = 0u;
    diag_mark_operation("diag_deinit");
    diag_set_presence(FY_PRESENT_UNKNOWN);
    diag_set_error("none");
    diag_set_response_kind("not requested");
    set_awg_status("unknown");
    Serial.printf("[fy diag] deinit serial2=no\r\n");
#endif
}

bool fy_diag_uart_reinit(void)
{
    fy_diag_uart_deinit();
    return fy_diag_uart_init();
}

size_t fy_diag_flush_rx(void)
{
    size_t count = 0;

#if FW_DISABLE_FY_HARDWARE
    diag_mark_operation("diag_flush");
    diag_set_error("FY hardware disabled in this variant");
    diag_set_response_kind("disabled");
    return 0u;
#else
    diag_mark_operation("diag_flush");
    if (!s_awg_serial_started) {
        diag_set_error("Serial2 not initialized");
        diag_set_response_kind("not initialized");
        Serial.printf("[fy diag] flush skipped serial2=no\r\n");
        return 0u;
    }
    while (s_awg_serial.available()) {
        s_awg_serial.read();
        count++;
    }
    diag_set_error("none");
    diag_set_response_kind("flushed");
    Serial.printf("[fy diag] flush rx_bytes=%u\r\n", (unsigned)count);
    return count;
#endif
}

int fy_diag_read_raw(uint16_t timeout_ms)
{
    uint8_t rx_buf[96];
    bool timed_out = false;
    bool saw_invalid = false;
    int n = 0;

    diag_mark_operation("diag_read");

#if FW_DISABLE_FY_HARDWARE
    diag_set_error("FY hardware disabled in this variant");
    diag_set_response_kind("disabled");
    return -1;
#else
    if (!s_awg_serial_started) {
        diag_set_error("Serial2 not initialized");
        diag_set_response_kind("not initialized");
        Serial.printf("[fy diag] read skipped serial2=no\r\n");
        return -1;
    }

    if (timeout_ms == 0u) timeout_ms = s_awg_timeout_ms;
    n = diag_capture_raw_response(rx_buf, sizeof(rx_buf), timeout_ms, &timed_out, &saw_invalid);
    if (n > 0) {
        diag_record_rx(rx_buf, (size_t)n, saw_invalid);
        if (saw_invalid) {
            diag_set_presence(FY_PRESENT_UNKNOWN);
            diag_set_error("response contains invalid/non-printable bytes");
            diag_set_response_kind("invalid");
            set_awg_status("invalid");
        } else if (response_is_lf_only_ack(rx_buf, (size_t)n)) {
            diag_set_presence(FY_PRESENT_PRESENT);
            diag_set_error("LF-only ACK");
            diag_set_response_kind("lf-only ack");
            set_awg_status("lf-only ack");
        } else if (response_has_printable_payload(rx_buf, (size_t)n)) {
            diag_set_presence(FY_PRESENT_PRESENT);
            diag_set_error("none");
            diag_set_response_kind("payload");
            set_awg_status("available");
        } else {
            diag_set_presence(FY_PRESENT_UNKNOWN);
            diag_set_error("response without printable payload");
            diag_set_response_kind("invalid");
            set_awg_status("invalid");
        }
    } else if (timed_out) {
        s_fy_diag.timeout_count++;
        diag_set_presence(FY_PRESENT_ABSENT);
        diag_set_error("timeout waiting for raw response");
        diag_set_response_kind("timeout");
        set_awg_status("timeout");
    } else {
        diag_set_presence(FY_PRESENT_ABSENT);
        diag_set_error("no response bytes");
        diag_set_response_kind("no response");
        set_awg_status("no response");
    }

    Serial.printf("[fy diag] read timeout=%u rx_len=%d invalid=%s mode=%s status=%s response=%s presence=%s raw=%s hex=%s\r\n",
        (unsigned)timeout_ms,
        n,
        saw_invalid ? "yes" : "no",
        s_awg_serial_mode,
        fy_status_text(),
        s_fy_diag.last_response_kind,
        diag_presence_label(s_fy_diag.fy_present_state),
        s_fy_diag.last_rx_ascii,
        s_fy_diag.last_rx_hex);
    return n;
#endif
}

int fy_diag_send_raw(const char *cmd, bool append_newline, uint16_t read_timeout_ms)
{
    char tx_buf[128];
    size_t cmd_len = 0u;

    diag_mark_operation("diag_send");

#if FW_DISABLE_FY_HARDWARE
    diag_set_error("FY hardware disabled in this variant");
    diag_set_response_kind("disabled");
    return -1;
#else
    if (!s_awg_serial_started) {
        diag_set_error("Serial2 not initialized");
        diag_set_response_kind("not initialized");
        Serial.printf("[fy diag] send skipped serial2=no\r\n");
        return -1;
    }
    if (cmd == NULL || cmd[0] == '\0') {
        diag_set_error("empty raw command");
        diag_set_response_kind("invalid");
        Serial.printf("[fy diag] send skipped empty command\r\n");
        return -1;
    }

    cmd_len = strlen(cmd);
    if (cmd_len >= sizeof(tx_buf)) cmd_len = sizeof(tx_buf) - 2u;
    memcpy(tx_buf, cmd, cmd_len);
    if (append_newline) {
        tx_buf[cmd_len++] = '\n';
    }
    tx_buf[cmd_len] = '\0';

    (void)serial_write_bytes((const uint8_t *)tx_buf, cmd_len);
    diag_set_error("none");
    Serial.printf("[fy diag] send tx_len=%u read_timeout=%u mode=%s raw=%s hex=%s\r\n",
        (unsigned)cmd_len,
        (unsigned)read_timeout_ms,
        s_awg_serial_mode,
        s_fy_diag.last_tx_raw,
        s_fy_diag.last_tx_hex);

    if (read_timeout_ms > 0u) {
        return fy_diag_read_raw(read_timeout_ms);
    }
    return 0;
#endif
}

bool fy_diag_probe_protocol(uint16_t timeout_ms, char *summary, size_t summary_len)
{
    char result[160];
    char parsed_umo[64];
    const char *parser_status = "not requested";
    uint16_t previous_timeout = s_awg_timeout_ms;
    static const char kProbeCmd[] = "UMO";
    int rx_len = 0;
    bool ok = false;

    if (summary != NULL && summary_len > 0u) {
        summary[0] = '\0';
    }

#if FW_DISABLE_FY_HARDWARE
    snprintf(result, sizeof(result), "protocol probe unavailable in %s", FW_VARIANT_NAME);
    set_manual_test_summary(result);
    if (summary != NULL && summary_len > 0u) {
        diag_copy_text(summary, summary_len, result);
    }
    diag_mark_operation("diag_probe");
    diag_set_error("FY hardware disabled in this variant");
    diag_set_response_kind("disabled");
    return false;
#else
    diag_mark_operation("diag_probe");
    if (!s_awg_serial_started && !fy_diag_uart_init()) {
        snprintf(result, sizeof(result), "serial2 init failed");
        set_manual_test_summary(result);
        if (summary != NULL && summary_len > 0u) {
            diag_copy_text(summary, summary_len, result);
        }
        diag_set_error("Serial2 init failed");
        return false;
    }

    if (timeout_ms == 0u) timeout_ms = s_awg_timeout_ms;
    s_awg_timeout_ms = timeout_ms;
    s_fy_diag.timeout_ms = timeout_ms;
    s_awg_serial.setTimeout(s_awg_timeout_ms);

    delay(10);
    while (s_awg_serial.available()) s_awg_serial.read();

    diag_set_presence(FY_PRESENT_UNKNOWN);
    diag_set_error("none");
    diag_set_response_kind("not requested");
    set_awg_status("probing");

    Serial.printf("[fy diag] probe request cmd=%s timeout=%u serial2=%s mode=%s status_before=%s\r\n",
        kProbeCmd,
        (unsigned)timeout_ms,
        s_awg_serial_started ? "yes" : "no",
        s_awg_serial_mode,
        fy_status_text());

    rx_len = fy_diag_send_raw(kProbeCmd, true, timeout_ms);
    diag_copy_text(s_fy_diag.last_umo_raw, sizeof(s_fy_diag.last_umo_raw), s_fy_diag.last_rx_ascii);
    parsed_umo[0] = '\0';
    if (rx_len > 0 && strcmp(s_fy_diag.last_response_kind, "payload") == 0) {
        ok = parse_umo_payload_from_escaped(s_fy_diag.last_rx_ascii,
            parsed_umo, sizeof(parsed_umo), &parser_status);
        diag_copy_text(s_fy_diag.last_umo_parsed, sizeof(s_fy_diag.last_umo_parsed), parsed_umo);
        diag_set_umo_parser_status(parser_status);
    } else {
        diag_copy_text(s_fy_diag.last_umo_parsed, sizeof(s_fy_diag.last_umo_parsed), "");
        if (strcmp(s_fy_diag.last_response_kind, "lf-only ack") == 0) {
            diag_set_umo_parser_status("lf-only ack");
        } else if (strcmp(s_fy_diag.last_response_kind, "timeout") == 0) {
            diag_set_umo_parser_status("timeout");
        } else if (strcmp(s_fy_diag.last_response_kind, "no response") == 0) {
            diag_set_umo_parser_status("no response");
        } else if (strcmp(s_fy_diag.last_response_kind, "invalid") == 0) {
            diag_set_umo_parser_status("invalid");
        } else {
            diag_set_umo_parser_status("no payload");
        }
    }
    s_awg_timeout_ms = previous_timeout;
    s_fy_diag.timeout_ms = s_awg_timeout_ms;
    s_awg_serial.setTimeout(s_awg_timeout_ms);

    snprintf(result, sizeof(result),
        "protocol_probe cmd=%s result=%s response=%s status=%s presence=%s serial2=%s",
        kProbeCmd,
        ok ? "success" : "fail",
        s_fy_diag.last_response_kind,
        fy_status_text(),
        diag_presence_label(s_fy_diag.fy_present_state),
        s_awg_serial_started ? "yes" : "no");
    set_manual_test_summary(result);
    if (summary != NULL && summary_len > 0u) {
        diag_copy_text(summary, summary_len, result);
    }

    Serial.printf("[fy diag] UMO raw=%s parsed=%s parser=%s\r\n",
        s_fy_diag.last_umo_raw[0] ? s_fy_diag.last_umo_raw : "(empty)",
        s_fy_diag.last_umo_parsed[0] ? s_fy_diag.last_umo_parsed : "(empty)",
        s_fy_diag.last_umo_parser_status[0] ? s_fy_diag.last_umo_parser_status : "unknown");
    Serial.printf("[fy diag] probe result=%s cmd=%s response=%s status=%s presence=%s tx_total=%lu rx_total=%lu error=%s\r\n",
        ok ? "success" : "fail",
        kProbeCmd,
        s_fy_diag.last_response_kind,
        fy_status_text(),
        diag_presence_label(s_fy_diag.fy_present_state),
        (unsigned long)s_fy_diag.tx_total_bytes,
        (unsigned long)s_fy_diag.rx_total_bytes,
        s_fy_diag.last_error);
    return ok;
#endif
}

void fy_diag_get_snapshot(FyDiagSnapshot *out)
{
    if (out == NULL) return;
    s_fy_diag.serial_initialized = s_awg_serial_started ? 1u : 0u;
    s_fy_diag.baud = s_awg_baud;
    s_fy_diag.timeout_ms = s_awg_timeout_ms;
    s_fy_diag.rx_pin = AWG_UART_RX_PIN;
    s_fy_diag.tx_pin = AWG_UART_TX_PIN;
    diag_copy_text(s_fy_diag.serial_mode, sizeof(s_fy_diag.serial_mode), s_awg_serial_mode);
    diag_copy_text(s_fy_diag.current_status, sizeof(s_fy_diag.current_status), s_awg_status);
    *out = s_fy_diag;
}

const char *fy_diag_presence_text(void)
{
    return diag_presence_label(s_fy_diag.fy_present_state);
}

bool fy_diag_set_serial_mode(const char *mode)
{
#if FW_DISABLE_FY_HARDWARE
    (void)mode;
    diag_set_error("FY hardware disabled in this variant");
    diag_set_response_kind("disabled");
    return false;
#else
    if (!fy_is_supported_serial_mode(mode)) {
        diag_set_error("unsupported serial mode");
        diag_set_response_kind("invalid");
        return false;
    }
    copy_serial_mode(s_awg_serial_mode, sizeof(s_awg_serial_mode), mode);
    s_awg_uart_config = serial_mode_to_uart_config(s_awg_serial_mode);
    diag_copy_text(s_fy_diag.serial_mode, sizeof(s_fy_diag.serial_mode), s_awg_serial_mode);
    if (s_awg_serial_started) {
        restart_awg_serial();
    }
    diag_mark_operation("diag_mode");
    diag_set_error("none");
    Serial.printf("[fy diag] serial mode set mode=%s serial2=%s\r\n",
        s_awg_serial_mode,
        s_awg_serial_started ? "yes" : "no");
    return true;
#endif
}

bool fy_diag_compare_8n2_vs_8n1(uint16_t timeout_ms, char *summary, size_t summary_len)
{
    char summary_8n2[192];
    char summary_8n1[192];
    char saved_mode[5];
    bool ok_8n2;
    bool ok_8n1 = false;

    if (summary != NULL && summary_len > 0u) {
        summary[0] = '\0';
    }

    diag_mark_operation("diag_compare");
    copy_serial_mode(saved_mode, sizeof(saved_mode), s_awg_serial_mode);

    if (!fy_diag_set_serial_mode("8N2")) {
        diag_copy_text(summary_8n2, sizeof(summary_8n2), "8N2 setup failed");
        ok_8n2 = false;
    } else {
        ok_8n2 = fy_diag_probe_protocol(timeout_ms, summary_8n2, sizeof(summary_8n2));
    }

    if (!ok_8n2) {
        if (!fy_diag_set_serial_mode("8N1")) {
            diag_copy_text(summary_8n1, sizeof(summary_8n1), "8N1 setup failed");
            ok_8n1 = false;
        } else {
            ok_8n1 = fy_diag_probe_protocol(timeout_ms, summary_8n1, sizeof(summary_8n1));
        }
    } else {
        diag_copy_text(summary_8n1, sizeof(summary_8n1), "not needed because 8N2 had valid payload");
    }

    (void)fy_diag_set_serial_mode(saved_mode);

    if (summary != NULL && summary_len > 0u) {
        snprintf(summary, summary_len,
            "8N2: %s | 8N1: %s | default=%s",
            summary_8n2,
            summary_8n1,
            ok_8n2 ? "8N2" : (ok_8n1 ? "8N1" : "8N2"));
    }

    Serial.printf("[fy diag] compare 8N2_vs_8N1 summary_8n2=%s summary_8n1=%s restore_mode=%s\r\n",
        summary_8n2,
        summary_8n1,
        saved_mode);
    return ok_8n2 || ok_8n1;
}

static bool fy_functional_transport_ok(void)
{
    return strcmp(s_awg_status, "timeout") != 0
        && strcmp(s_awg_status, "invalid") != 0
        && strcmp(s_awg_status, "no response") != 0
        && strcmp(s_awg_status, "disabled") != 0
        && strcmp(s_fy_diag.last_response_kind, "timeout") != 0
        && strcmp(s_fy_diag.last_response_kind, "invalid") != 0
        && strcmp(s_fy_diag.last_response_kind, "no response") != 0;
}

static bool fy_functional_scpi_write(const char *label, const char *cmd,
    char *detail, size_t detail_len)
{
    char cmd_buf[128];
    bool accepted;

    if (detail != NULL && detail_len > 0u) {
        detail[0] = '\0';
    }
    if (cmd == NULL || cmd[0] == '\0') {
        diag_set_error("empty functional scpi command");
        if (detail != NULL && detail_len > 0u) {
            diag_copy_text(detail, detail_len, "empty command");
        }
        return false;
    }

    diag_mark_operation("functional_scpi");
    strncpy(cmd_buf, cmd, sizeof(cmd_buf) - 1);
    cmd_buf[sizeof(cmd_buf) - 1] = '\0';
    Serial.printf("[fy final] step=%s scpi_write=%s\r\n", label, cmd_buf);

    accepted = scpi_handle_write(cmd_buf);
    if (!accepted) {
        diag_set_error("SCPI parser rejected command");
        if (detail != NULL && detail_len > 0u) {
            snprintf(detail, detail_len, "%s parser rejected", label);
        }
        return false;
    }
    if (!fy_functional_transport_ok()) {
        if (detail != NULL && detail_len > 0u) {
            snprintf(detail, detail_len, "%s transport fail status=%s response=%s error=%s",
                label,
                fy_status_text(),
                s_fy_diag.last_response_kind,
                s_fy_diag.last_error);
        }
        return false;
    }
    if (detail != NULL && detail_len > 0u) {
        snprintf(detail, detail_len, "%s ok status=%s response=%s",
            label,
            fy_status_text(),
            s_fy_diag.last_response_kind);
    }
    Serial.printf("[fy final] step=%s result=%s error=%s\r\n",
        label,
        detail != NULL && detail[0] ? detail : "ok",
        s_fy_diag.last_error[0] ? s_fy_diag.last_error : "none");
    return true;
}

static bool fy_functional_scpi_query(const char *cmd, char *reply, size_t reply_len,
    char *detail, size_t detail_len)
{
    char cmd_buf[64];
    uint32_t declared_len = 0;
    ScpiReadMode mode = SCPI_READ_NONE;
    int rx_len;

    if (reply != NULL && reply_len > 0u) {
        reply[0] = '\0';
    }
    if (detail != NULL && detail_len > 0u) {
        detail[0] = '\0';
    }
    if (cmd == NULL || cmd[0] == '\0') {
        diag_set_error("empty functional query");
        if (detail != NULL && detail_len > 0u) {
            diag_copy_text(detail, detail_len, "empty query");
        }
        return false;
    }

    diag_mark_operation("functional_query");
    strncpy(cmd_buf, cmd, sizeof(cmd_buf) - 1);
    cmd_buf[sizeof(cmd_buf) - 1] = '\0';
    Serial.printf("[fy final] query=%s\r\n", cmd_buf);

    if (!scpi_handle_write(cmd_buf)) {
        diag_set_error("SCPI parser rejected query");
        if (detail != NULL && detail_len > 0u) {
            snprintf(detail, detail_len, "query rejected cmd=%s", cmd_buf);
        }
        return false;
    }

    rx_len = scpi_read_response(reply, (int)reply_len, &declared_len, &mode);
    if (rx_len <= 0 || mode == SCPI_READ_NONE) {
        diag_set_error("no SCPI readback available");
        if (detail != NULL && detail_len > 0u) {
            snprintf(detail, detail_len, "query empty cmd=%s", cmd_buf);
        }
        return false;
    }

    while (rx_len > 0 && (reply[rx_len - 1] == '\r' || reply[rx_len - 1] == '\n')) {
        reply[--rx_len] = '\0';
    }
    diag_set_parser_readback(reply);
    if (detail != NULL && detail_len > 0u) {
        snprintf(detail, detail_len, "query ok mode=%u declared=%lu reply=%s",
            (unsigned)mode,
            (unsigned long)declared_len,
            reply);
    }
    Serial.printf("[fy final] query reply=%s mode=%u declared=%lu\r\n",
        reply,
        (unsigned)mode,
        (unsigned long)declared_len);
    return true;
}

void fy_functional_reset_state(void)
{
    diag_mark_operation("functional_reset");
    fy_reset_cache();
    fy_reset_sent_state();
    fy_reset_functional_diag_state();
    set_manual_test_summary("not requested");

#if FW_DISABLE_FY_HARDWARE
    diag_set_error("FY hardware disabled in this variant");
    diag_set_response_kind("disabled");
    set_awg_status("disabled");
#else
    if (s_awg_serial_started) {
        while (s_awg_serial.available()) {
            s_awg_serial.read();
        }
    }
    diag_set_presence(FY_PRESENT_UNKNOWN);
    diag_set_error("none");
    diag_set_response_kind("not requested");
    set_awg_status(s_awg_enabled ? "pending" : "disabled");
    Serial.printf("[fy final] software state reset serial2=%s fy_enabled=%s\r\n",
        s_awg_serial_started ? "yes" : "no",
        s_awg_enabled ? "yes" : "no");
#endif
}

bool fy_functional_test_run(uint16_t timeout_ms, char *summary, size_t summary_len)
{
    char detail[224];
    char query_reply[160];
    bool previous_enabled = s_awg_enabled;
    bool ok = false;

    if (summary != NULL && summary_len > 0u) {
        summary[0] = '\0';
    }

#if !FW_ENABLE_FY_FINAL_TEST
    diag_mark_operation("functional_test");
    diag_set_error("functional test unavailable in this variant");
    diag_set_sequence_result("functional test unavailable");
    if (summary != NULL && summary_len > 0u) {
        diag_copy_text(summary, summary_len, "functional test unavailable");
    }
    return false;
#else
    fy_functional_reset_state();
    diag_mark_operation("functional_test");
    if (timeout_ms == 0u) timeout_ms = s_awg_timeout_ms;
    diag_set_sequence_result("running");

    if (!s_awg_serial_started && !fy_diag_uart_init()) {
        diag_set_sequence_result("L1 fail: serial2 init failed");
        if (summary != NULL && summary_len > 0u) {
            diag_copy_text(summary, summary_len, s_fy_diag.last_sequence_result);
        }
        set_manual_test_summary(s_fy_diag.last_sequence_result);
        return false;
    }

    s_fy_diag.last_functional_level = 1u;
    s_awg_enabled = true;

    Serial.printf("[fy final] functional test start variant=%s timeout=%u mode=%s serial2=%s\r\n",
        FW_VARIANT_NAME,
        (unsigned)timeout_ms,
        s_awg_serial_mode,
        s_awg_serial_started ? "yes" : "no");

    if (!fy_diag_probe_protocol(timeout_ms, detail, sizeof(detail))) {
        snprintf(detail, sizeof(detail), "L2 fail: UMO probe %s", s_fy_diag.last_umo_parser_status);
        diag_set_sequence_result(detail);
        goto done;
    }
    s_fy_diag.last_functional_level = 2u;
    Serial.printf("[fy final] UMO raw=%s\r\n",
        s_fy_diag.last_umo_raw[0] ? s_fy_diag.last_umo_raw : "(empty)");
    Serial.printf("[fy final] UMO parsed=%s parser=%s\r\n",
        s_fy_diag.last_umo_parsed[0] ? s_fy_diag.last_umo_parsed : "(empty)",
        s_fy_diag.last_umo_parser_status[0] ? s_fy_diag.last_umo_parser_status : "unknown");

    if (strcmp(s_fy_diag.last_umo_parser_status, "payload valid") != 0) {
        snprintf(detail, sizeof(detail), "L3 fail: UMO parser %s", s_fy_diag.last_umo_parser_status);
        diag_set_sequence_result(detail);
        goto done;
    }
    s_fy_diag.last_functional_level = 3u;

    if (!fy_functional_scpi_write("c1_outp_off", "C1:OUTP OFF", detail, sizeof(detail))) {
        snprintf(detail, sizeof(detail), "L4 fail: %s", s_fy_diag.last_error);
        diag_set_sequence_result(detail);
        goto done;
    }
    if (!fy_functional_scpi_write("c2_outp_off", "C2:OUTP OFF", detail, sizeof(detail))) {
        snprintf(detail, sizeof(detail), "L4 fail: %s", s_fy_diag.last_error);
        diag_set_sequence_result(detail);
        goto done;
    }
    if (!fy_functional_scpi_write("c1_bswv_set",
            "C1:BSWV WVTP,SINE,FRQ,1000,AMP,1,OFST,0,PHSE,0",
            detail, sizeof(detail))) {
        snprintf(detail, sizeof(detail), "L4 fail: %s", s_fy_diag.last_error);
        diag_set_sequence_result(detail);
        goto done;
    }
    s_fy_diag.last_functional_level = 4u;

    if (!fy_functional_scpi_query("C1:BSWV?", query_reply, sizeof(query_reply),
            detail, sizeof(detail))) {
        snprintf(detail, sizeof(detail), "L5 fail: %s", s_fy_diag.last_error);
        diag_set_sequence_result(detail);
        goto done;
    }
    s_fy_diag.last_functional_level = 5u;
    s_fy_diag.last_functional_success = 1u;
    snprintf(detail, sizeof(detail), "functional ok level=5 UMO=%s readback=%s",
        s_fy_diag.last_umo_parsed[0] ? s_fy_diag.last_umo_parsed : "-",
        query_reply[0] ? query_reply : "-");
    diag_set_sequence_result(detail);
    ok = true;

done:
    s_awg_enabled = previous_enabled;
    if (!ok) {
        s_fy_diag.last_functional_success = 0u;
    }
    if (summary != NULL && summary_len > 0u) {
        diag_copy_text(summary, summary_len, s_fy_diag.last_sequence_result);
    }
    set_manual_test_summary(s_fy_diag.last_sequence_result);
    Serial.printf("[fy final] functional verdict=%s level=%u parser=%s sequence=%s\r\n",
        ok ? "success" : "fail",
        (unsigned)s_fy_diag.last_functional_level,
        s_fy_diag.last_umo_parser_status[0] ? s_fy_diag.last_umo_parser_status : "unknown",
        s_fy_diag.last_sequence_result[0] ? s_fy_diag.last_sequence_result : "(empty)");
    return ok;
#endif
}

bool fy_manual_safe_test(char *summary, size_t summary_len)
{
#if FW_DISABLE_FY_HARDWARE
    Serial.printf("[fy manual] request ignored: hardware disabled in variant=%s\r\n", FW_VARIANT_NAME);
    return false;
#else
    return fy_diag_probe_protocol(s_awg_timeout_ms, summary, summary_len);
#endif
}

int fy_read_available(char *buf, int buf_max)
{
    bool saw_invalid = false;
    int n = read_filtered_serial(buf, buf_max, &saw_invalid);
    if (n > 0) {
        diag_record_rx((const uint8_t *)buf, (size_t)n, saw_invalid);
    } else if (saw_invalid) {
        s_fy_diag.invalid_response_count++;
        diag_set_error("invalid FY bytes while reading");
    }
    if (saw_invalid) {
        diag_tracef("[awg rx] ignored invalid ttl bytes\r\n");
    }
    return n;
}

void fy_set_output(uint8_t ch, uint8_t on)
{
    char buf[8];
    if (ch == 1) g_awg.ch1_on = on;
    else g_awg.ch2_on = on;

    if (s_awg_sent_valid) {
        uint8_t sent = (ch == 1) ? s_awg_sent.ch1_on : s_awg_sent.ch2_on;
        if (sent == on) return;
    }

    int n = snprintf(buf, sizeof(buf), "W%cN%c\n", ch_letter(ch), on ? '1' : '0');
    if (n <= 0 || n >= (int)sizeof(buf)) return;
    if (!send_cmd(buf, (size_t)n)) return;
    if (ch == 1) s_awg_sent.ch1_on = on;
    else s_awg_sent.ch2_on = on;
    s_awg_sent_valid = true;
}

void fy_set_wave(uint8_t ch, AwgWaveType wave)
{
    char buf[10];
    if (ch == 1) g_awg.ch1_wave = wave;
    else g_awg.ch2_wave = wave;

    if (s_awg_sent_valid) {
        AwgWaveType sent = (ch == 1) ? s_awg_sent.ch1_wave : s_awg_sent.ch2_wave;
        if (sent == wave) return;
    }

    int n = snprintf(buf, sizeof(buf), "W%cW%02u\n", ch_letter(ch), (unsigned)wave);
    if (n <= 0 || n >= (int)sizeof(buf)) return;
    if (!send_cmd(buf, (size_t)n)) return;
    if (ch == 1) s_awg_sent.ch1_wave = wave;
    else s_awg_sent.ch2_wave = wave;
    s_awg_sent_valid = true;
}

void fy_set_freq(uint8_t ch, double freq_hz)
{
    char buf[28];
    if (ch == 1) g_awg.ch1_freq_hz = freq_hz;
    else g_awg.ch2_freq_hz = freq_hz;

    if (s_awg_sent_valid) {
        double sent = (ch == 1) ? s_awg_sent.ch1_freq_hz : s_awg_sent.ch2_freq_hz;
        if (same_value(sent, freq_hz, 0.0000005)) return;
    }

    int n = fy_format_freq_cmd(buf, sizeof(buf), ch, freq_hz);
    if (n <= 0 || n >= (int)sizeof(buf)) return;
    if (!send_cmd(buf, (size_t)n)) return;
    if (ch == 1) s_awg_sent.ch1_freq_hz = freq_hz;
    else s_awg_sent.ch2_freq_hz = freq_hz;
    s_awg_sent_valid = true;
}

void fy_set_ampl(uint8_t ch, double ampl_v)
{
    char buf[16];
    if (ch == 1) g_awg.ch1_ampl_v = ampl_v;
    else g_awg.ch2_ampl_v = ampl_v;

    fy_update_drive_values(ch);

    if (s_awg_sent_valid) {
        double sent = (ch == 1) ? s_awg_sent.ch1_drive_ampl_v : s_awg_sent.ch2_drive_ampl_v;
        double drive = (ch == 1) ? g_awg.ch1_drive_ampl_v : g_awg.ch2_drive_ampl_v;
        if (same_value(sent, drive, 0.00005)) return;
    }

    double drive = (ch == 1) ? g_awg.ch1_drive_ampl_v : g_awg.ch2_drive_ampl_v;
    int n = snprintf(buf, sizeof(buf), "W%cA%.4f\n", ch_letter(ch), drive);
    if (n <= 0 || n >= (int)sizeof(buf)) return;
    if (!send_cmd(buf, (size_t)n)) return;
    if (ch == 1) {
        s_awg_sent.ch1_ampl_v = ampl_v;
        s_awg_sent.ch1_drive_ampl_v = drive;
    } else {
        s_awg_sent.ch2_ampl_v = ampl_v;
        s_awg_sent.ch2_drive_ampl_v = drive;
    }
    s_awg_sent_valid = true;
}

void fy_set_offset(uint8_t ch, double ofst_v)
{
    char buf[16];
    if (ch == 1) g_awg.ch1_ofst_v = ofst_v;
    else g_awg.ch2_ofst_v = ofst_v;

    fy_update_drive_values(ch);

    if (s_awg_sent_valid) {
        double sent = (ch == 1) ? s_awg_sent.ch1_drive_ofst_v : s_awg_sent.ch2_drive_ofst_v;
        double drive = (ch == 1) ? g_awg.ch1_drive_ofst_v : g_awg.ch2_drive_ofst_v;
        if (same_value(sent, drive, 0.0005)) return;
    }

    double drive = (ch == 1) ? g_awg.ch1_drive_ofst_v : g_awg.ch2_drive_ofst_v;
    int n = snprintf(buf, sizeof(buf), "W%cO%.3f\n", ch_letter(ch), drive);
    if (n <= 0 || n >= (int)sizeof(buf)) return;
    if (!send_cmd(buf, (size_t)n)) return;
    if (ch == 1) {
        s_awg_sent.ch1_ofst_v = ofst_v;
        s_awg_sent.ch1_drive_ofst_v = drive;
    } else {
        s_awg_sent.ch2_ofst_v = ofst_v;
        s_awg_sent.ch2_drive_ofst_v = drive;
    }
    s_awg_sent_valid = true;
}

void fy_set_phase(uint8_t ch, double phase_deg)
{
    char buf[16];
    if (ch == 1) g_awg.ch1_phase_deg = phase_deg;
    else g_awg.ch2_phase_deg = phase_deg;

    if (s_awg_sent_valid) {
        double sent = (ch == 1) ? s_awg_sent.ch1_phase_deg : s_awg_sent.ch2_phase_deg;
        if (same_value(sent, phase_deg, 0.0005)) return;
    }

    int n = snprintf(buf, sizeof(buf), "WFP%.3f\n", phase_deg);
    if (n <= 0 || n >= (int)sizeof(buf)) return;
    if (!send_cmd(buf, (size_t)n)) return;
    if (ch == 1) s_awg_sent.ch1_phase_deg = phase_deg;
    else s_awg_sent.ch2_phase_deg = phase_deg;
    s_awg_sent_valid = true;
}

void fy_set_load(uint8_t ch, AwgLoadMode load)
{
    AwgLoadMode current = (ch == 1) ? g_awg.ch1_load : g_awg.ch2_load;

    if (current == load) return;
    if (ch == 1) g_awg.ch1_load = load;
    else g_awg.ch2_load = load;

    fy_update_drive_values(ch);
    if (ch == 1) s_awg_sent.ch1_load = load;
    else s_awg_sent.ch2_load = load;

    fy_set_ampl(ch, (ch == 1) ? g_awg.ch1_ampl_v : g_awg.ch2_ampl_v);
    fy_set_offset(ch, (ch == 1) ? g_awg.ch1_ofst_v : g_awg.ch2_ofst_v);
}

AwgLoadMode fy_get_load(uint8_t ch)
{
    return (ch == 1) ? g_awg.ch1_load : g_awg.ch2_load;
}

double fy_get_drive_ampl(uint8_t ch)
{
    return (ch == 1) ? g_awg.ch1_drive_ampl_v : g_awg.ch2_drive_ampl_v;
}

double fy_get_drive_offset(uint8_t ch)
{
    return (ch == 1) ? g_awg.ch1_drive_ofst_v : g_awg.ch2_drive_ofst_v;
}

const char *fy_load_to_text(AwgLoadMode load)
{
    switch (load) {
    case AWG_LOAD_50: return "50";
    case AWG_LOAD_75: return "75";
    case AWG_LOAD_HIZ: return "HZ";
    default: return "?";
    }
}

static const struct {
    const char *siglent;
    AwgWaveType fy;
} s_wave_map[] = {
    { "SINE",     AWG_SINE     },
    { "SQUARE",   AWG_SQUARE   },
    { "RAMP",     AWG_POSRAMP  },
    { "TRIANGLE", AWG_TRIANGLE },
    { "NOISE",    AWG_NOISE    },
    { NULL,       AWG_SINE     }
};

AwgWaveType fy_wave_from_siglent(const char *name)
{
    for (int i = 0; s_wave_map[i].siglent != NULL; i++) {
        if (strcasecmp(name, s_wave_map[i].siglent) == 0) {
            return s_wave_map[i].fy;
        }
    }
    return AWG_SINE;
}

const char *fy_wave_to_siglent(AwgWaveType w)
{
    for (int i = 0; s_wave_map[i].siglent != NULL; i++) {
        if (s_wave_map[i].fy == w) {
            return s_wave_map[i].siglent;
        }
    }
    return "SINE";
}

int fy_raw_cmd(const char *cmd, char *reply_buf, int reply_max)
{
    bool saw_invalid = false;

    if (!s_awg_enabled) {
        set_awg_status("disabled");
        return -1;
    }

    while (s_awg_serial.available()) s_awg_serial.read();
    awg_trace_tx(cmd, strlen(cmd));
    diag_tracef("\r\n");
    (void)serial_write_bytes((const uint8_t *)cmd, strlen(cmd));
    (void)serial_write_bytes((const uint8_t *)"\n", 1u);
    delay(200);

    int n = read_filtered_serial(reply_buf, reply_max, &saw_invalid);
    if (n > 0) {
        diag_record_rx((const uint8_t *)reply_buf, (size_t)n, saw_invalid);
        set_awg_status("available");
        diag_set_presence(FY_PRESENT_PRESENT);
    } else if (saw_invalid) {
        s_fy_diag.invalid_response_count++;
        diag_set_error("invalid FY response / noise on RX");
        diag_set_presence(FY_PRESENT_UNKNOWN);
        set_awg_status("invalid");
    } else {
        s_fy_diag.timeout_count++;
        diag_set_error("timeout waiting for fy_raw_cmd response");
        diag_set_presence(FY_PRESENT_ABSENT);
        set_awg_status("timeout");
    }
    if (saw_invalid) {
        diag_tracef("[awg rx] ignored invalid ttl bytes\r\n");
    }
    return n;
}
