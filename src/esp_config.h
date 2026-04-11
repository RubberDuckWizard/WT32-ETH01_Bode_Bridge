#ifndef ESP_CONFIG_H
#define ESP_CONFIG_H

#if !defined(FW_RELEASE) && !defined(FW_RELEASE_FINAL_SAFE) && !defined(FW_DIAG_UART0) && !defined(FW_DIAG_ETH) && !defined(FW_DIAG_PROTOCOL) && !defined(FW_BRINGUP_SAFE) && !defined(FW_DIAG_FY_SAFE) && !defined(FW_FINAL_TEST_SAFE)
  #define FW_RELEASE_FINAL_SAFE 1
#endif

#if defined(FW_RELEASE_FINAL_SAFE)
  #define ENABLE_UART_CONSOLE        0
  #define ENABLE_AWG_AUTOSTREAM      1
  #define ENABLE_TRACE_RUNTIME       0
  #define ENABLE_ETH_RUNTIME         1
  #define ENABLE_ETH_DIAG            0
  #define ENABLE_PROTOCOL_DIAG       0
  #define ENABLE_MINIMAL_LAN_NTP     1
  #define FW_VARIANT_NAME            "release_final_safe"
  #define FW_FORCE_RECOVERY_AP       0
  #define FW_START_AP_DURING_STA_BOOT 1
  #define FW_SAFE_RECOVERY_FALLBACK  1
  #define FW_IGNORE_STORED_CONFIG    0
  #define FW_SKIP_FY_INIT            0
  #define FW_DISABLE_FY_HARDWARE     0
  #define FW_ENABLE_MANUAL_FY_TEST   0
  #define FW_ENABLE_FY_SERIAL_DIAG   0
  #define FW_ENABLE_FY_FINAL_TEST    0
  #define FW_DEFER_FY_INIT           1
  #define FW_KEEP_RECOVERY_AP_ALWAYS_ON 1
#elif defined(FW_FINAL_TEST_SAFE)
  #define ENABLE_UART_CONSOLE        1
  #define ENABLE_AWG_AUTOSTREAM      0
  #define ENABLE_TRACE_RUNTIME       0
  #define ENABLE_ETH_RUNTIME         0
  #define ENABLE_ETH_DIAG            0
  #define ENABLE_PROTOCOL_DIAG       0
  #define ENABLE_MINIMAL_LAN_NTP     0
  #define FW_VARIANT_NAME            "final_test_safe"
  #define FW_FORCE_RECOVERY_AP       1
  #define FW_START_AP_DURING_STA_BOOT 0
  #define FW_SAFE_RECOVERY_FALLBACK  1
  #define FW_IGNORE_STORED_CONFIG    1
  #define FW_SKIP_FY_INIT            1
  #define FW_DISABLE_FY_HARDWARE     0
  #define FW_ENABLE_MANUAL_FY_TEST   1
  #define FW_ENABLE_FY_SERIAL_DIAG   1
  #define FW_ENABLE_FY_FINAL_TEST    1
#elif defined(FW_DIAG_FY_SAFE)
  #define ENABLE_UART_CONSOLE        1
  #define ENABLE_AWG_AUTOSTREAM      0
  #define ENABLE_TRACE_RUNTIME       0
  #define ENABLE_ETH_RUNTIME         0
  #define ENABLE_ETH_DIAG            0
  #define ENABLE_PROTOCOL_DIAG       0
  #define ENABLE_MINIMAL_LAN_NTP     0
  #define FW_VARIANT_NAME            "diag_fy_safe"
  #define FW_FORCE_RECOVERY_AP       1
  #define FW_START_AP_DURING_STA_BOOT 0
  #define FW_SAFE_RECOVERY_FALLBACK  1
  #define FW_IGNORE_STORED_CONFIG    1
  #define FW_SKIP_FY_INIT            1
  #define FW_DISABLE_FY_HARDWARE     0
  #define FW_ENABLE_MANUAL_FY_TEST   1
  #define FW_ENABLE_FY_SERIAL_DIAG   1
  #define FW_ENABLE_FY_FINAL_TEST    0
#elif defined(FW_BRINGUP_SAFE)
  #define ENABLE_UART_CONSOLE        0
  #define ENABLE_AWG_AUTOSTREAM      0
  #define ENABLE_TRACE_RUNTIME       0
  #define ENABLE_ETH_RUNTIME         0
  #define ENABLE_ETH_DIAG            0
  #define ENABLE_PROTOCOL_DIAG       0
  #define ENABLE_MINIMAL_LAN_NTP     0
  #define FW_VARIANT_NAME            "bringup_safe"
  #define FW_FORCE_RECOVERY_AP       1
  #define FW_START_AP_DURING_STA_BOOT 0
  #define FW_SAFE_RECOVERY_FALLBACK  1
  #define FW_IGNORE_STORED_CONFIG    1
  #define FW_SKIP_FY_INIT            1
  #define FW_DISABLE_FY_HARDWARE     1
  #define FW_ENABLE_MANUAL_FY_TEST   0
  #define FW_ENABLE_FY_SERIAL_DIAG   0
  #define FW_ENABLE_FY_FINAL_TEST    0
#elif defined(FW_DIAG_PROTOCOL)
  #define ENABLE_UART_CONSOLE        0
  #define ENABLE_AWG_AUTOSTREAM      1
  #define ENABLE_TRACE_RUNTIME       0
  #define ENABLE_ETH_RUNTIME         0
  #define ENABLE_ETH_DIAG            0
  #define ENABLE_PROTOCOL_DIAG       1
  #define ENABLE_MINIMAL_LAN_NTP     0
  #define FW_VARIANT_NAME            "diag_protocol"
  #define FW_FORCE_RECOVERY_AP       0
  #define FW_START_AP_DURING_STA_BOOT 0
  #define FW_SAFE_RECOVERY_FALLBACK  0
  #define FW_IGNORE_STORED_CONFIG    0
  #define FW_SKIP_FY_INIT            0
  #define FW_DISABLE_FY_HARDWARE     0
  #define FW_ENABLE_MANUAL_FY_TEST   0
  #define FW_ENABLE_FY_SERIAL_DIAG   0
  #define FW_ENABLE_FY_FINAL_TEST    0
#elif defined(FW_DIAG_ETH)
  #define ENABLE_UART_CONSOLE        1
  #define ENABLE_AWG_AUTOSTREAM      0
  #define ENABLE_TRACE_RUNTIME       1
  #define ENABLE_ETH_RUNTIME         1
  #define ENABLE_ETH_DIAG            1
  #define ENABLE_PROTOCOL_DIAG       0
  #define ENABLE_MINIMAL_LAN_NTP     0
  #define FW_VARIANT_NAME            "diag_eth"
  #define FW_FORCE_RECOVERY_AP       0
  #define FW_START_AP_DURING_STA_BOOT 0
  #define FW_SAFE_RECOVERY_FALLBACK  0
  #define FW_IGNORE_STORED_CONFIG    0
  #define FW_SKIP_FY_INIT            0
  #define FW_DISABLE_FY_HARDWARE     0
  #define FW_ENABLE_MANUAL_FY_TEST   0
  #define FW_ENABLE_FY_SERIAL_DIAG   0
  #define FW_ENABLE_FY_FINAL_TEST    0
#elif defined(FW_DIAG_UART0)
  #define ENABLE_UART_CONSOLE        1
  #define ENABLE_AWG_AUTOSTREAM      0
  #define ENABLE_TRACE_RUNTIME       1
  #define ENABLE_ETH_RUNTIME         0
  #define ENABLE_ETH_DIAG            0
  #define ENABLE_PROTOCOL_DIAG       0
  #define ENABLE_MINIMAL_LAN_NTP     0
  #define FW_VARIANT_NAME            "diag_uart0"
  #define FW_FORCE_RECOVERY_AP       0
  #define FW_START_AP_DURING_STA_BOOT 0
  #define FW_SAFE_RECOVERY_FALLBACK  0
  #define FW_IGNORE_STORED_CONFIG    0
  #define FW_SKIP_FY_INIT            0
  #define FW_DISABLE_FY_HARDWARE     0
  #define FW_ENABLE_MANUAL_FY_TEST   0
  #define FW_ENABLE_FY_SERIAL_DIAG   0
  #define FW_ENABLE_FY_FINAL_TEST    0
#else
  #define ENABLE_UART_CONSOLE        0
  #define ENABLE_AWG_AUTOSTREAM      1
  #define ENABLE_TRACE_RUNTIME       0
  #define ENABLE_ETH_RUNTIME         0
  #define ENABLE_ETH_DIAG            0
  #define ENABLE_PROTOCOL_DIAG       0
  #define ENABLE_MINIMAL_LAN_NTP     0
  #define FW_VARIANT_NAME            "release"
  #define FW_FORCE_RECOVERY_AP       0
  #define FW_START_AP_DURING_STA_BOOT 0
  #define FW_SAFE_RECOVERY_FALLBACK  1
  #define FW_IGNORE_STORED_CONFIG    0
  #define FW_SKIP_FY_INIT            0
  #define FW_DISABLE_FY_HARDWARE     0
  #define FW_ENABLE_MANUAL_FY_TEST   0
  #define FW_ENABLE_FY_SERIAL_DIAG   0
  #define FW_ENABLE_FY_FINAL_TEST    0
  #define FW_DEFER_FY_INIT           1
#endif

#if !defined(FW_DEFER_FY_INIT)
  #define FW_DEFER_FY_INIT           0
#endif

#if !defined(FW_KEEP_RECOVERY_AP_ALWAYS_ON)
  #define FW_KEEP_RECOVERY_AP_ALWAYS_ON 0
#endif

#if !defined(ENABLE_RUNTIME_HEARTBEAT)
  #if ENABLE_UART_CONSOLE || ENABLE_TRACE_RUNTIME
    #define ENABLE_RUNTIME_HEARTBEAT 1
  #else
    #define ENABLE_RUNTIME_HEARTBEAT 0
  #endif
#endif

#if !defined(ENABLE_VERBOSE_VXI_LOGS)
  #if ENABLE_PROTOCOL_DIAG || ENABLE_TRACE_RUNTIME
    #define ENABLE_VERBOSE_VXI_LOGS 1
  #else
    #define ENABLE_VERBOSE_VXI_LOGS 0
  #endif
#endif

#define FW_BUILD_STRING __DATE__ " " __TIME__

/* Default runtime configuration */
#define DEF_USE_DHCP                   0
#define DEF_IP                         10, 11, 13, 221
#define DEF_MASK                       255, 255, 255, 0
#define DEF_GW                         10, 11, 13, 1
#define DEF_DNS                        10, 11, 13, 1
#define DEF_DNS2                       10, 11, 13, 1
#define DEF_LAN_IP                     10, 11, 13, 221
#define DEF_LAN_MASK                   255, 255, 255, 0
#define DEF_SCOPE_IP                   10, 11, 13, 220
#define DEF_SCOPE_PORT                 80U
#define DEF_SCOPE_HTTP_PROXY_ENABLE    1
#define DEF_SCOPE_CONNECT_TIMEOUT_MS   1200U
#define DEF_SCOPE_PROBE_INTERVAL_MS    5000U
#define DEF_VXI_SESSION_TIMEOUT_MS     8000U
#define DEF_AWG_SERIAL_TIMEOUT_MS      1200U
#define DEF_AUTO_OUTPUT_OFF_TIMEOUT_MS 30000U
#define DEF_RECOVERY_AP_ENABLE         1
#define STA_CONNECT_TIMEOUT_MS         15000U
#define STA_RETRY_INTERVAL_MS          15000U
#if defined(FW_RELEASE_FINAL_SAFE)
  #define STA_BOOT_CONNECT_DELAY_MS    4000U
  #define STA_CONNECT_MAX_ATTEMPTS     3U
#else
  #define STA_BOOT_CONNECT_DELAY_MS    0U
  #define STA_CONNECT_MAX_ATTEMPTS     0U
#endif
#define NTP_SYNC_RETRY_INTERVAL_MS     30000U
#define NTP_VALID_UNIX_THRESHOLD       1700000000UL

#define LAN_NTP_PORT                   123U

#define SCOPE_HTTP_PROXY_LISTEN_PORT   100U
#define SCOPE_HTTP_PROXY_TARGET_PORT    80U
#define SCOPE_HTTP_PROXY_MAX_CONNECTIONS 12U
#define SCOPE_HTTP_PROXY_MAX_UPSTREAMS  4U
#define SCOPE_HTTP_PROXY_IDLE_TIMEOUT_MS 15000U
#define SCOPE_HTTP_PROXY_BUFFER_SIZE   256U
#define SCOPE_HTTP_PROXY_SNIFF_BYTES   768U

#define SCOPE_VNC_PROXY_LISTEN_PORT    5900U
#define SCOPE_VNC_PROXY_TARGET_PORT    5900U
#define SCOPE_VNC_PROXY_MAX_CONNECTIONS 2U
#define SCOPE_VNC_PROXY_IDLE_TIMEOUT_MS 20000U
#define SCOPE_VNC_PROXY_BUFFER_SIZE    384U
#define SCOPE_VNC_PROXY_SNIFF_BYTES    512U

#define DEF_WIFI_SSID                  ""
#define DEF_WIFI_PASSWORD              ""
#define DEF_RECOVERY_AP_SSID           RECOVERY_AP_SSID
#if FW_FORCE_RECOVERY_AP
  #define DEF_RECOVERY_AP_PASSWORD     ""
#else
  #define DEF_RECOVERY_AP_PASSWORD     "wt32-bode"
#endif

#define DEF_DEVICE_HOSTNAME       "wt32-bode"
#define DEF_FRIENDLY_NAME         "WT32 Bode Bridge"
#define DEF_IDN_RESPONSE_NAME     "SDG1062X"
#define DEF_AWG_SERIAL_MODE       "8N2"

#define RECOVERY_AP_SSID          "WT32-BODE-SETUP"
#define RECOVERY_AP_CHANNEL       1
#define RECOVERY_AP_MAX_CLIENTS   4
#define RECOVERY_AP_MIN_PASSWORD_LEN 8U
#define RECOVERY_AP_IP            192, 168, 4, 1
#define RECOVERY_AP_MASK          255, 255, 255, 0

#define AWG_FW_FAMILY_OLDER        0u
#define AWG_FW_FAMILY_LATER_FY6900 1u
#define DEF_AWG_FW_FAMILY          AWG_FW_FAMILY_LATER_FY6900

#define BODE_FIXED_ID_STRING      "IDN-SGLT-PRI SDG0000X"
#define IDN_RESPONSE_PREFIX       "IDN-SGLT-PRI "

/* RPC / VXI-11 port numbers */
#define RPC_PORT        111
#define VXI_PORT_A      9009
#define VXI_PORT_B      9010
#define VXI_STABLE_PORT VXI_PORT_A

/* RPC / XDR constants */
#define PORTMAP_PROG    0x000186A0UL
#define VXI11_CORE_PROG 0x000607AFUL
#define PORTMAP_VERS    2U
#define VXI11_CORE_VERS 1U

#define PORTMAP_GETPORT     3U
#define VXI11_CREATE_LINK   10U
#define VXI11_DEVICE_WRITE  11U
#define VXI11_DEVICE_READ   12U
#define VXI11_DESTROY_LINK  23U

#define RPC_CALL            0U
#define RPC_REPLY           1U
#define MSG_ACCEPTED        0U
#define SUCCESS_STAT        0U
#define RPC_SINGLE_FRAG     0x80000000UL

/* Buffer sizes */
#define RX_BUF_SIZE     288
#define TX_BUF_SIZE     128
#define CMD_BUF_SIZE    256
#define RESP_BUF_SIZE   128

/* WT32-ETH01 timing and UART mapping */
#define ETH_LINK_TIMEOUT_MS       5000U
#define ETH_IP_TIMEOUT_MS        15000U
#define AWG_UART_PORT_NUM         2
#define AWG_UART_RX_PIN           5
#define AWG_UART_TX_PIN          17

/* FY6900 defaults */
#define AWG_BAUD_RATE            115200

#define MIN_SCOPE_TIMEOUT_MS      200U
#define MAX_SCOPE_TIMEOUT_MS    30000U
#define MIN_SCOPE_INTERVAL_MS    1000U
#define MAX_SCOPE_INTERVAL_MS   60000U
#define MIN_VXI_TIMEOUT_MS       1000U
#define MAX_VXI_TIMEOUT_MS      60000U
#define MIN_AWG_TIMEOUT_MS        100U
#define MAX_AWG_TIMEOUT_MS      10000U
#define MIN_AUTO_OFF_TIMEOUT_MS  1000U
#define MAX_AUTO_OFF_TIMEOUT_MS 60000U

#ifdef DEBUG_BUILD
  #define DBG_INIT()    do { Serial.begin(115200); delay(10); } while(0)
  #define DBG(x)        Serial.println(x)
  #define DBGF(...)     Serial.printf(__VA_ARGS__)
#else
  #define DBG_INIT()
  #define DBG(x)
  #define DBGF(...)
#endif

#endif /* ESP_CONFIG_H */
