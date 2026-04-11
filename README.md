# WT32-ETH01 Bode Bridge

Firmware for a WT32-ETH01 module that bridges a Siglent `SDS800X-HD` oscilloscope and a FeelElec/FeelTech `FY6900` function generator for Bode measurements. The same device also exposes the oscilloscope web interface through an HTTP proxy and a noVNC/VNC WebSocket proxy while keeping a recovery-oriented management UI available on Ethernet, WiFi STA, and a local AP.

This repository is intentionally centered on one primary public build and two auxiliary service variants:

- primary public build: `wt32eth_release_final_safe`
- auxiliary recovery build: `wt32eth_bringup_safe`
- auxiliary FY6900 service build: `wt32eth_final_test_safe`

## What This Project Does

- Presents the FY6900 to the oscilloscope Bode workflow over `UART2`
- Maintains a dedicated Ethernet LAN between the WT32-ETH01 and the oscilloscope
- Provides a configurable WiFi STA uplink for normal network access
- Keeps a recovery AP available for configuration and service access
- Hosts a browser-based configuration and diagnostics UI
- Offers a minimal LAN-only NTP service for the oscilloscope side
- Proxies the oscilloscope web UI on `http://<esp32-ip>:100/`
- Proxies the oscilloscope noVNC/VNC WebSocket path on `ws://<esp32-ip>:5900/websockify`

## Target Hardware

- WT32-ETH01 ESP32 Ethernet module
- Siglent `SDS800X-HD` oscilloscope on the dedicated Ethernet LAN
- FeelElec / FeelTech `FY6900` connected to `UART2`

UART wiring used by the firmware:

- `IO17` (`TXD2`) -> `FY6900 RX`
- `IO5` (`RXD2`) <- `FY6900 TX`
- Shared ground between the WT32-ETH01 and the FY6900 is required

## Kept Build Variants

### `wt32eth_release_final_safe`

Primary public release build and the recommended firmware for normal use.

- Ethernet scope LAN
- WiFi STA
- Permanent recovery AP
- Minimal LAN NTP service
- Bode/VXI services
- Normal FY6900 runtime support
- Scope HTTP proxy on port `100`
- Scope noVNC/VNC proxy on port `5900`

### `wt32eth_bringup_safe`

Auxiliary recovery build. It ignores stored configuration, forces the recovery AP at boot, and keeps FY hardware disabled during boot. Use it when you need to regain safe configuration access without depending on the full runtime stack.

### `wt32eth_final_test_safe`

Auxiliary FY6900 service build. It forces the recovery AP at boot, does not start the full normal runtime, and keeps FY tests manual from the web UI and diagnostics path. Use it for controlled UART2/FY6900 validation and troubleshooting.

## Network Architecture

### Dedicated Scope LAN

- Default WT32 LAN IP: `10.11.13.221/24`
- Default scope IP: `10.11.13.220`
- Gateway: `0.0.0.0`
- DNS: `0.0.0.0`

This interface is dedicated to the oscilloscope link. Bode/VXI traffic, scope probing, and the minimal NTP server depend on this LAN, not on the WiFi STA uplink.

### WiFi STA

- Configured from the web UI and stored in NVS
- Supports DHCP or static addressing
- Provides a second management path when connected

### Recovery AP

- Default SSID: `WT32-BODE-SETUP`
- Default IP: `192.168.4.1/24`
- Main build default password: `wt32-bode`
- Safe recovery and service builds force AP-first behavior

## Build, Flash, And Release Assets

- Source build instructions: [BUILDING.md](BUILDING.md)
- Flashing instructions: [FLASHING.md](FLASHING.md)
- Validation summary: [docs/VALIDATION_SUMMARY.md](docs/VALIDATION_SUMMARY.md)
- Release artifact guide: [release/README.md](release/README.md)

Primary release artifact:

- `release/wt32eth_release_final_safe/app.bin`

Supporting precompiled files for clean manual flashing:

- `release/wt32eth_release_final_safe/bootloader.bin`
- `release/wt32eth_release_final_safe/partitions.bin`

Auxiliary precompiled build artifacts:

- `release/wt32eth_bringup_safe/app.bin`
- `release/wt32eth_final_test_safe/app.bin`

Main build:

```bash
pio run -e wt32eth_release_final_safe
```

Auxiliary builds:

```bash
pio run -e wt32eth_bringup_safe
pio run -e wt32eth_final_test_safe
```

Generate the tracked release assets:

```bash
python scripts/build_bins.py
```

Upload from source with PlatformIO:

```bash
pio run -e wt32eth_release_final_safe -t upload --upload-port COMx
```

Serial monitor:

```bash
pio device monitor -b 115200 -p COMx
```

## How To Use It

1. Build or obtain the `wt32eth_release_final_safe` release files.
2. Flash the firmware to the WT32-ETH01.
3. If automatic bootloader entry fails, hold the board in boot mode and perform a manual hardware reset during flashing.
4. Connect the oscilloscope to the dedicated Ethernet side and keep it in the same subnet as the configured scope LAN.
5. Connect to one of the available management paths:
   - `http://192.168.4.1/` through the recovery AP
   - `http://<wifi-sta-ip>/` when WiFi STA is connected
   - `http://<lan-ip>/` on the dedicated LAN when Ethernet is up
6. Configure network, scope, and FY6900 parameters in the UI.
7. Verify the proxied scope UI on `http://<esp32-ip>:100/`.
8. Verify noVNC/WebSocket access through the proxied scope UI.

Useful web routes:

- `/` runtime summary
- `/network` LAN, WiFi STA, and AP configuration
- `/scope` scope and proxy configuration
- `/fy6900` UART2/FY configuration
- `/bode` Bode parameters
- `/diag` service diagnostics
- `/save`
- `/reboot`
- `/factory-reset`

The `/diag` endpoint is intentionally retained as a service tool.

## Precompiled Binary Usage

Use `release/wt32eth_release_final_safe/app.bin` when the module already has a compatible bootloader and partition layout. Use the accompanying `bootloader.bin` and `partitions.bin` files for a clean manual flash workflow. Exact commands and caveats are documented in [FLASHING.md](FLASHING.md) and [release/README.md](release/README.md).

## Known Real Limitations

- Two simultaneous noVNC clients are validated; a third client is not a supported target.
- No TLS or `wss://` is implemented.
- Long-duration stress testing for concurrent noVNC sessions is not yet documented as complete.
- Automatic bootloader entry is not guaranteed on every board; manual intervention may be required for flashing.
- The firmware assumes correct power stability, Ethernet cabling, UART wiring, and shared ground. It does not compensate in software for wiring or power faults.

## Power, Wiring, And Recovery Notes

- Use a stable power source for the WT32-ETH01 and the FY6900.
- Keep a common ground between the WT32-ETH01 and FY6900.
- Keep the oscilloscope on the dedicated LAN defined in the configuration.
- If the main build becomes unreachable, use `wt32eth_bringup_safe` for recovery.
- If FY6900 communication must be isolated and inspected, use `wt32eth_final_test_safe`.

## Licensing

This repository is released under the MIT License. See [LICENSE](LICENSE) for the project license and [NOTICE_AND_THIRD_PARTY_LICENSES.md](NOTICE_AND_THIRD_PARTY_LICENSES.md) for third-party licensing notes related to the toolchain and generated binaries.

## Repository Hygiene Notes

Local vendor reference files in `DOC/` and raw engineering reports in `debug/` are intentionally excluded from the public Git history. Their useful conclusions are consolidated into the tracked English documentation in this repository.