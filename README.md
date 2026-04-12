# WT32-ETH01 Bode Bridge
# pio device monitor -e wt32eth_release_final_safe -b 115200

Final firmware for `WT32-ETH01` that presents an `FY6900` to the BODE function of a Siglent `SDS800X-HD` oscilloscope and, through the same device, exposes the oscilloscope web UI via HTTP proxy and noVNC.

This repository has been conservatively cleaned up around a single main variant:

- main publishable build: `wt32eth_release_final_safe`
- auxiliary recovery build: `wt32eth_bringup_safe`
- auxiliary FY service build: `wt32eth_final_test_safe`

The authoritative documents for the final state are this README and `debug/final_cleanup_report.txt`.

## Final architecture

- dedicated static LAN for the oscilloscope on WT32-ETH01 Ethernet
- no gateway and no DNS on the dedicated LAN interface
- separate WiFi STA, configurable as DHCP or static
- permanently active AP for recovery and local UI access
- Web UI accessible from LAN, WiFi STA, and AP
- minimal NTP server on LAN
- BODE/VXI services started on LAN
- FY6900 on UART2
- HTTP proxy for the scope UI on port `100`
- noVNC/VNC proxy on port `5900`

## Kept builds

### `wt32eth_release_final_safe`

The main build. It keeps all of the following active at the same time:

- Ethernet LAN for the scope
- WiFi STA
- permanent AP
- NTP server on LAN
- BODE/VXI
- FY6900 active in normal runtime
- HTTP/noVNC proxy for the scope UI

### `wt32eth_bringup_safe`

Recovery build. It forces AP, ignores the stored configuration, and does not initialize FY6900 during boot. It is kept for quickly returning the device to a configurable state if the main runtime must be isolated.

### `wt32eth_final_test_safe`

Service build for FY6900. It forces AP, does not start the normal final runtime, and keeps FY tests manual only from UI/diag. It is kept for hardware validation and UART2/FY troubleshooting without changing the main build.

## Final network

### LAN for the oscilloscope

- default WT32 LAN static IP: `10.11.13.221/24`
- default scope IP: `10.11.13.220`
- scope HTTP probe port: `80`
- LAN gateway: `0.0.0.0`
- LAN DNS: `0.0.0.0`

The LAN is dedicated to the WT32 <-> oscilloscope relationship. BODE/VXI and the NTP server on LAN depend on this interface, not on WiFi STA.

### WiFi STA

- configured from the UI and stored in `Preferences / NVS`
- can operate in DHCP or static mode
- the Web UI remains available on STA when the connection exists

### Permanent AP

- default SSID: `WT32-BODE-SETUP`
- default IP: `192.168.4.1/24`
- active in the main final build as a recovery and service path

## FY6900

### Pin map

- `UART0` flash/log: `IO1 = TX0`, `IO3 = RX0`
- `UART2` FY6900: `IO17 = TXD2` to `FY6900 RX`, `IO5 = RXD2` from `FY6900 TX`

### Final electrical and protocol settings

- default baud rate: `115200`
- default serial mode: `8N2`
- default serial timeout: `1200 ms`
- flow control: `none`
- command format: ASCII text commands terminated with `LF` / `0x0A`
- FY replies are also terminated with `LF` / `0x0A`

### Recommended FY6900 terminal settings

For direct manual communication tests with FY6900, use:

- baud rate: `115200`
- data bits: `8`
- stop bits: `2`
- parity: `none`
- flow control: `none`
- line ending for transmitted commands: `Append LF`
- received polling interval: any reasonable terminal default is acceptable; `100 ms` works in the validated setup
- local echo: optional; useful for terminal testing, but not required by the protocol

### Recommended FY6900 test commands

Use simple read-only commands first:

- `UMO` + `LF` -> reads the model string
- `UID` + `LF` -> reads the instrument ID
- `RMN` + `LF` -> reads the main output enable status

For write commands, always keep the final `LF`. Example:

- `WMN0` + `LF` -> disables the main output

### Robust behavior

- the main build uses `deferred fy_init`, so the network and Web UI come up before FY initialization
- an absent or slow FY must not block safe boot
- the `wt32eth_final_test_safe` service build keeps manual FY tests for controlled verification
- the validated project setup uses `115200 8N2` for FY communication; this is intentionally different from the ESP32 ROM bootloader UART settings used for firmware flashing

## Proxy for the oscilloscope UI

### HTTP proxy

- listens on `http://<ESP32_IP>:100/`
- forwards to `scope_ip:80`
- sufficient for the oscilloscope’s normal web pages

### noVNC / VNC proxy

- listens on `ws://<ESP32_IP>:5900/websockify`
- transparently forwards to `scope_ip:5900`
- the current implementation has been validated for 2 simultaneous noVNC clients

### Remaining real limitations

- 2 simultaneous noVNC clients are demonstrated
- a third client is not a supported target in the current state
- there is no TLS or `wss://`
- there is no complex multiplexing; each client uses its own upstream connection to the scope

## Web UI

The useful routes kept in the final variant are:

- `/` runtime status and summary
- `/network` LAN, WiFi STA, and AP configuration
- `/scope` scope and proxy configuration
- `/fy6900` UART2/FY configuration
- `/bode` BODE parameters
- `/diag` service diagnostics
- `/save` save configuration
- `/reboot` controlled restart
- `/factory-reset` reset configuration to defaults

The `/diag` endpoint is intentionally kept as a service tool. It is not a temporary leftover.

## Build and flash

### Main build

```bash
pio run -e wt32eth_release_final_safe
```

### Main upload

```bash
pio run -e wt32eth_release_final_safe -t upload --upload-port COMx
```

### Auxiliary builds

```bash
pio run -e wt32eth_bringup_safe
pio run -e wt32eth_final_test_safe
```

### Serial monitor

```bash
pio device monitor -b 115200 -p COMx
```

### Generate binaries copied to the project root

```bash
python scripts/build_bins.py
```



## Programming, power, and UART-level notes

### Entering bootloader mode for flashing

For manual flashing, hold `GPIO0` low during power-up, or keep `GPIO0` low and briefly pull `EN` to `GND` to reset the board into the ROM bootloader. This matches the normal ESP32 download-boot flow and is commonly used on WT32-ETH01 boards.

### Recommended USB-TTL programming connection

Use a **3.3 V logic USB-TTL adapter** such as a CP2102-based adapter and cross the UART0 data lines:

- `WT32 TXD / IO1 / TXD0` -> adapter `RX`
- `WT32 RXD / IO3 / RXD0` -> adapter `TX`
- `WT32 GND` -> adapter `GND`

Optional manual control lines, if your adapter and wiring support them:

- adapter `RTS` -> `EN`
- adapter `DTR` -> `GPIO0`

These lines are not mandatory, but they can help with automatic reset/bootloader entry on some setups.

### ESP32 ROM bootloader serial settings (for flashing)

When talking to the ESP32 ROM bootloader through a USB-TTL adapter, use:

- baud rate: `115200`
- data bits: `8`
- stop bits: `1`
- parity: `none`
- flow control: `none`

These are the recommended serial settings for the ESP32 bootloader connection. Higher data-transfer baud rates can be used by flashing tools after the initial connection, but the safe baseline remains `115200 8N1`.

### Serial monitor settings after flashing

For normal runtime logs on `UART0`, use:

- baud rate: `115200`
- data bits: `8`
- stop bits: `1`
- parity: `none`
- flow control: `none`

If a terminal program keeps control lines asserted and accidentally holds the ESP32 in reset or boot mode, disable hardware flow control and close any active monitor before starting a flash operation.

### Power supply requirements

The WT32-ETH01 supports **either 5 V or 3.3 V input power (choose one, not both)**, and the module datasheet specifies a **minimum 500 mA supply capability**. For this project, a **solid 3.3 V supply is recommended** when available; otherwise 5 V input is also valid. In practice, using a **1 A-capable supply** gives more margin during Ethernet, WiFi, and proxy activity.

### Practical decoupling recommendation

For reliable flashing and runtime stability, add a **470 uF low-ESR/polymer capacitor** between `3.3V` and `GND`, with the shortest possible leads. This is a practical project recommendation, not a WT32-ETH01 datasheet requirement.

### Observed current on this project setup

On the validated project hardware, the observed normal runtime current was about **300 mA**, with **very short peaks up to roughly 1.2 A for a few milliseconds**. Treat this as an observed project characteristic, not a guaranteed WT32-ETH01 datasheet value.

### FY6900 UART voltage-level caution

The **programming adapter must use 3.3 V logic**. The WT32/ESP32 GPIO voltage tolerance is **3.6 V max**, so the `FY6900 TX -> WT32 RX` path must also be **3.3 V-safe**. If your FY6900 serial TX level is above 3.3 V, add a level shifter or divider before feeding `IO5`.

### Important distinction: programmer UART vs FY6900 UART

These are two different serial links with different settings:

#### 1. Programmer / bootloader / runtime logs on UART0

- pins: `IO1 / TX0`, `IO3 / RX0`
- purpose:
  - flashing firmware
  - boot messages
  - runtime serial logs
- safe default settings:
  - flashing: `115200 8N1`
  - runtime monitor: `115200 8N1`

#### 2. FY6900 control link on UART2

- pins: `IO17 / TXD2`, `IO5 / RXD2`
- purpose:
  - generator control
  - FY protocol commands
- validated project settings:
  - `115200 8N2`
  - `LF` line ending
  - `none` parity
  - `none` flow control

Do not mix these two UART links or their settings.

## Recommended minimum test

1. build `wt32eth_release_final_safe`
2. flash it to the board
3. if auto-reset does not enter the bootloader, you must put the board into boot mode and perform a manual hardware reset
4. check on serial that LAN, STA/AP, and proxy statuses appear
5. check the UI on one of the active addresses
6. check BODE/NTP on LAN
7. check `http://<ESP32_IP>:100/`
8. check noVNC through the proxied scope UI

## What is demonstrated and what is not

Demonstrated in the workspace and in the retained validation sessions:

- the `wt32eth_release_final_safe` build passes
- the `wt32eth_bringup_safe` build passes
- the `wt32eth_final_test_safe` build passes
- FY6900 communicates on UART2 in the service build
- the HTTP proxy on `:100` works
- noVNC through `:5900` works with 2 simultaneous clients

Not automatically demonstrated by the code alone:

- all physical power and cabling scenarios on your hardware
- long-term stability for 2 simultaneous noVNC sessions
- any future upload without physical intervention; if auto-reset does not work, it must be done manually

## Power and hardware notes

- the project assumes a correct common ground between WT32 and FY6900
- there is no code-level compensation for unstable power, external brownout, or incorrect serial cabling
- if upload or boot no longer enters automatically, local physical intervention remains the user’s responsibility

## Useful files kept in `debug/`

- `wt32eth_release_final_safe_report.txt`
- `wt32eth_final_test_safe_report.txt`
- `scope_http_proxy_report.txt`
- `siglent_novnc_proxy_report.txt`
- `novnc_two_clients_upgrade_report.txt`
- `final_cleanup_report.txt`

These are historical engineering reports. The current final state of the project is summarized in this README and in `debug/final_cleanup_report.txt`.
