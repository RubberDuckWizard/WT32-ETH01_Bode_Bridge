#ifndef ESP_FY6900_H
#define ESP_FY6900_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    AWG_SINE      = 0,
    AWG_SQUARE    = 1,
    AWG_TRIANGLE  = 5,
    AWG_POSRAMP   = 6,
    AWG_NEGRAMP   = 7,
    AWG_NOISE     = 25,
    AWG_WAVE_LAST
} AwgWaveType;

typedef enum {
    AWG_LOAD_50 = 0,
    AWG_LOAD_75,
    AWG_LOAD_HIZ
} AwgLoadMode;

typedef struct {
    uint8_t    ch1_on;
    uint8_t    ch2_on;
    AwgWaveType ch1_wave;
    AwgWaveType ch2_wave;
    double     ch1_freq_hz;
    double     ch2_freq_hz;
    double     ch1_ampl_v;
    double     ch2_ampl_v;
    double     ch1_ofst_v;
    double     ch2_ofst_v;
    double     ch1_drive_ampl_v;
    double     ch2_drive_ampl_v;
    double     ch1_drive_ofst_v;
    double     ch2_drive_ofst_v;
    double     ch1_phase_deg;
    double     ch2_phase_deg;
    AwgLoadMode ch1_load;
    AwgLoadMode ch2_load;
} AwgState;

typedef struct {
    uint8_t serial_initialized;
    uint8_t fy_present_state; /* 0=unknown, 1=present, 2=absent */
    uint32_t baud;
    uint16_t timeout_ms;
    uint8_t rx_pin;
    uint8_t tx_pin;
    uint32_t tx_total_bytes;
    uint32_t rx_total_bytes;
    uint32_t timeout_count;
    uint32_t invalid_response_count;
    uint32_t operation_count;
    uint32_t last_operation_ms;
    char current_status[24];
    char serial_mode[5];
    char last_response_kind[24];
    char last_operation[24];
    char last_tx_raw[128];
    char last_tx_hex[256];
    char last_rx_raw[128];
    char last_rx_ascii[128];
    char last_rx_hex[256];
    char last_error[96];
    char last_umo_raw[128];
    char last_umo_parsed[64];
    char last_umo_parser_status[32];
    char last_sequence_result[192];
    char last_parser_readback[160];
    uint8_t last_functional_level;
    uint8_t last_functional_success;
} FyDiagSnapshot;

extern AwgState g_awg;

void fy_init(void);
bool fy_is_enabled(void);
const char *fy_status_text(void);
bool fy_is_supported_baud(uint32_t baud);
bool fy_is_supported_serial_mode(const char *mode);
bool fy_is_supported_firmware_family(uint8_t family);
void fy_set_enabled(bool enabled);
uint32_t fy_get_baud(void);
const char *fy_get_serial_mode(void);
uint16_t fy_get_timeout_ms(void);
void fy_set_baud(uint32_t baud);
void fy_apply_runtime_config(void);
int  fy_read_available(char *buf, int buf_max);
void fy_force_resync(void);
void fy_restore_startup_state(void);
bool fy_manual_safe_test(char *summary, size_t summary_len);
bool fy_serial_initialized(void);
const char *fy_last_manual_test_summary(void);
bool fy_diag_uart_init(void);
void fy_diag_uart_deinit(void);
bool fy_diag_uart_reinit(void);
size_t fy_diag_flush_rx(void);
int fy_diag_send_raw(const char *cmd, bool append_newline, uint16_t read_timeout_ms);
int fy_diag_read_raw(uint16_t timeout_ms);
bool fy_diag_probe_protocol(uint16_t timeout_ms, char *summary, size_t summary_len);
void fy_diag_get_snapshot(FyDiagSnapshot *out);
const char *fy_diag_presence_text(void);
bool fy_diag_set_serial_mode(const char *mode);
bool fy_diag_compare_8n2_vs_8n1(uint16_t timeout_ms, char *summary, size_t summary_len);
void fy_functional_reset_state(void);
bool fy_functional_test_run(uint16_t timeout_ms, char *summary, size_t summary_len);

void fy_set_output (uint8_t ch, uint8_t on);
void fy_set_wave   (uint8_t ch, AwgWaveType wave);
void fy_set_freq   (uint8_t ch, double freq_hz);
void fy_set_ampl   (uint8_t ch, double ampl_v);
void fy_set_offset (uint8_t ch, double ofst_v);
void fy_set_phase  (uint8_t ch, double phase_deg);
void fy_set_load   (uint8_t ch, AwgLoadMode load);
AwgLoadMode fy_get_load(uint8_t ch);
double fy_get_drive_ampl(uint8_t ch);
double fy_get_drive_offset(uint8_t ch);
const char *fy_load_to_text(AwgLoadMode load);

AwgWaveType fy_wave_from_siglent(const char *name);
const char *fy_wave_to_siglent (AwgWaveType w);

int fy_raw_cmd(const char *cmd, char *reply_buf, int reply_max);

#endif /* ESP_FY6900_H */
