# Release Artifacts

## Primary Runtime

Main public build:

- `wt32eth_release_final_safe/app.bin`

Supporting files for clean manual flashing:

- `wt32eth_release_final_safe/bootloader.bin`
- `wt32eth_release_final_safe/partitions.bin`

## Auxiliary Builds

- `wt32eth_bringup_safe/app.bin`
  Recovery-oriented auxiliary build
- `wt32eth_final_test_safe/app.bin`
  FY6900 service-oriented auxiliary build

## Which File To Use

- Use `wt32eth_release_final_safe/app.bin` for normal updates when the target already has a compatible bootloader and partition table.
- Use the primary build's `bootloader.bin`, `partitions.bin`, and `app.bin` together for a clean manual flash.
- Use the auxiliary build binaries only when you explicitly need recovery or FY6900 service behavior.

## Checksums

`SHA256SUMS.txt` contains SHA-256 hashes for the binary artifacts exported by the packaging script.

## Final Runtime Policy

- the final release keeps normal `Preferences` / `NVS` writes user-driven
- identical saves are treated as no-op saves and do not rewrite flash
- boot-time automatic writes are reserved for legacy migration, failed config load recovery, or a critical repair of invalid stored configuration

## Final Default Network Roles

- WiFi STA defaults to DHCP
- the dedicated scope-side LAN stays static on `10.11.13.221/24`
- the recovery AP stays static on `192.168.4.1/24`
- these defaults avoid an immediate STA/LAN subnet conflict on clean `NVS`

## Current Validation Snapshot

- `wt32eth_release_final_safe` was rebuilt, flashed on `COM6`, and boot-logged on `2026-04-13`
- the final Web UI responded live at `http://192.168.91.157/`
- Web UI save tests confirmed both `No changes saved` for unchanged values and `Configuration saved` for a real change followed by restore
- the detailed summary is kept in [../docs/VALIDATION_SUMMARY.md](../docs/VALIDATION_SUMMARY.md)

## Flashing Reference

See [../FLASHING.md](../FLASHING.md) for exact flashing commands and boot/recovery notes.
