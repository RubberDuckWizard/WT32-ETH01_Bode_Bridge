# Building

## Requirements

- PlatformIO Core 6.1 or newer
- Python 3 with PlatformIO available as `pio`, `platformio`, or `python -m platformio`
- A Windows, Linux, or macOS environment able to build Espressif32 Arduino projects

## Supported Build Environments

- `wt32eth_release_final_safe`
  Main public firmware with Ethernet scope LAN, WiFi STA, permanent recovery AP, NTP, Bode/VXI, FY6900 runtime support, and scope HTTP/noVNC proxy support.
- `wt32eth_bringup_safe`
  Recovery build that ignores stored configuration, forces the AP at boot, and keeps FY hardware disabled during boot.
- `wt32eth_final_test_safe`
  FY6900 service build for manual diagnostics and controlled UART2 validation.

## Standard Build Commands

Main release build:

```bash
pio run -e wt32eth_release_final_safe
```

Recovery build:

```bash
pio run -e wt32eth_bringup_safe
```

FY6900 service build:

```bash
pio run -e wt32eth_final_test_safe
```

## Release Artifact Generation

Generate local release-staging assets after the builds succeed:

```bash
python scripts/build_bins.py -e wt32eth_release_final_safe
```

Clean and rebuild before exporting release assets:

```bash
python scripts/build_bins.py -e wt32eth_release_final_safe --clean
```

The script copies the selected firmware files into the local `release/` staging directory and also writes `release/SHA256SUMS.txt`.

These generated binaries are intended for GitHub Release assets and are ignored by Git. They should not be committed back into the source tree.

Generated structure:

- `release/wt32eth_release_final_safe/app.bin`
- `release/wt32eth_release_final_safe/bootloader.bin`
- `release/wt32eth_release_final_safe/partitions.bin`
- `release/SHA256SUMS.txt`

## Validation Baseline

The release preparation on April 11, 2026 validated all three kept PlatformIO environments successfully:

- `wt32eth_release_final_safe`
- `wt32eth_bringup_safe`
- `wt32eth_final_test_safe`

Build sizes and retained validation notes are summarized in [docs/VALIDATION_SUMMARY.md](docs/VALIDATION_SUMMARY.md).
