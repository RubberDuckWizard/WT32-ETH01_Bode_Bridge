# Validation Summary

## Release Preparation Checkpoint

Date: 2026-04-11

## Verified PlatformIO Builds

All kept environments were rebuilt successfully during release preparation:

- `wt32eth_release_final_safe`
  RAM: `62124` bytes
  Flash: `947705` bytes
- `wt32eth_bringup_safe`
  RAM: `61632` bytes
  Flash: `886325` bytes
- `wt32eth_final_test_safe`
  RAM: `61928` bytes
  Flash: `918885` bytes

## Retained Behavioral Evidence

The public repository does not include the raw local engineering notes from `debug/`, but their relevant conclusions were retained in the documentation set:

- The HTTP proxy on port `100` was previously validated against the oscilloscope web UI.
- The noVNC/VNC proxy on port `5900` was previously validated.
- Two simultaneous noVNC clients are the documented tested limit.
- FY6900 communication over `UART2` was previously validated through the service workflow.

## Not Repeated In This Packaging Pass

- A fresh live flash to physical hardware
- A new end-to-end live check of Ethernet, WiFi STA, AP, NTP, Bode, HTTP proxy, and noVNC on the actual bench setup
- Long-duration stress testing for concurrent noVNC sessions

## Local-Only Materials Intentionally Excluded From Git

- Vendor reference files under `DOC/`
- Raw engineering and debug notes under `debug/`

Those files remain useful locally but were excluded from the public Git history to keep the repository focused, English-only, and free of third-party reference material that is not required for building or using the firmware.
