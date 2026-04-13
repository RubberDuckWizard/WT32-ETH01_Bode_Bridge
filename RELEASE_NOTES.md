# Release Notes

## v0.2.0

Date: 2026-04-13

### Scope

This release refreshes the published firmware artifacts for the final primary build `wt32eth_release_final_safe` and keeps the auxiliary safe builds aligned with the current repository state. It also rolls forward the latest UI/runtime consistency fixes and the final README documentation update that clarifies the real bench wiring, LAN/WiFi roles, and release usage.

This pass also finalizes the persistence policy for the main release build:

- WiFi STA now defaults to DHCP on clean configuration
- the dedicated LAN keeps the `10.11.13.221/24` default separately from WiFi STA
- normal `Preferences` / `NVS` writes stay user-driven
- unchanged saves are explicit no-op saves instead of redundant flash rewrites
- boot-time automatic writes are limited to migration, failed-load recovery, or critical configuration repair

### Included Build Variants

- `wt32eth_release_final_safe`
  Primary production-oriented build with Ethernet scope LAN, WiFi STA, permanent recovery AP, configurable NTP upstream, Bode/VXI, FY6900 runtime support, and scope HTTP/noVNC proxy support.
- `wt32eth_bringup_safe`
  Recovery-oriented auxiliary build for AP-first safe access.
- `wt32eth_final_test_safe`
  FY6900 service-oriented auxiliary build for manual diagnostics.

### Release Artifacts

- `release/wt32eth_release_final_safe/app.bin`
- `release/wt32eth_release_final_safe/bootloader.bin`
- `release/wt32eth_release_final_safe/partitions.bin`
- `release/wt32eth_bringup_safe/app.bin`
- `release/wt32eth_final_test_safe/app.bin`
- `release/SHA256SUMS.txt`

### Validation Summary

Release refresh validation rebuilt all kept environments successfully on April 13, 2026:

- `wt32eth_release_final_safe`
  RAM: `62156` bytes
  Flash: `951113` bytes
- `wt32eth_bringup_safe`
  RAM: `61664` bytes
  Flash: `889609` bytes
- `wt32eth_final_test_safe`
  RAM: `61968` bytes
  Flash: `922241` bytes

Observed artifact hashes are documented in [release/SHA256SUMS.txt](release/SHA256SUMS.txt), and the local validation baseline is summarized in [docs/VALIDATION_SUMMARY.md](docs/VALIDATION_SUMMARY.md).

Additional final-pass validation for `wt32eth_release_final_safe` on April 13, 2026:

- clean rebuild succeeded
- erase + flash on `COM6` succeeded
- first boot after erase was captured on the real `WT32-ETH01`
- the final Web UI responded live at `http://192.168.91.157/`
- a real no-op save and a real changed save were both verified through the Web UI

### Known Limits

- Two simultaneous noVNC clients remain the documented validated limit.
- No TLS or `wss://` support is included.
- Physical flashing may still require manual boot/reset intervention depending on the board setup.

## Initial Public Release

Date: 2026-04-11

### Scope

This release packages the WT32-ETH01 firmware as a publishable GitHub repository with an English documentation set, a permissive project license, and tracked release artifacts for the kept firmware variants.

### Included Build Variants

- `wt32eth_release_final_safe`
  Primary production-oriented build with Ethernet scope LAN, WiFi STA, permanent recovery AP, NTP, Bode/VXI, FY6900 runtime support, and scope HTTP/noVNC proxy support.
- `wt32eth_bringup_safe`
  Recovery-oriented auxiliary build for AP-first safe access.
- `wt32eth_final_test_safe`
  FY6900 service-oriented auxiliary build for manual diagnostics.

### Release Artifacts

- `release/wt32eth_release_final_safe/app.bin`
- `release/wt32eth_release_final_safe/bootloader.bin`
- `release/wt32eth_release_final_safe/partitions.bin`
- `release/wt32eth_bringup_safe/app.bin`
- `release/wt32eth_final_test_safe/app.bin`
- `release/SHA256SUMS.txt`

### Validation Summary

Release preparation validated successful builds for all three kept environments on April 11, 2026:

- `wt32eth_release_final_safe`
- `wt32eth_bringup_safe`
- `wt32eth_final_test_safe`

Observed artifact sizes are documented in [docs/VALIDATION_SUMMARY.md](docs/VALIDATION_SUMMARY.md).

### Known Limits

- Two simultaneous noVNC clients are the validated limit in the current documented state.
- No TLS or `wss://` support is included.
- Physical flashing may still require manual boot/reset intervention depending on the board setup.
