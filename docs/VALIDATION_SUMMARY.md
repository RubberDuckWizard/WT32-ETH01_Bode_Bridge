# Validation Summary

## Final Release Validation

Date: 2026-04-13
Target build: `wt32eth_release_final_safe`

## Configuration Policy Audit

- `saveConfig()` now skips physical `Preferences` / `NVS` writes when the current configuration matches the last persisted snapshot.
- no periodic autosave, background save, or runtime save loop was found in the audited code paths.
- boot-time automatic writes were removed from the audited release path.
- missing, incomplete, incompatible, or invalid stored configuration is now repaired only in RAM until the user explicitly saves.

## Final Network Defaults

- WiFi STA default: DHCP enabled (`DEF_USE_DHCP = 1`)
- WiFi STA default static tuple: unset / zeroed until the user explicitly configures one
- dedicated LAN default: `10.11.13.221/24`
- default scope target: `10.11.13.220`
- recovery AP default: `192.168.4.1/24`
- clean-NVS first boot therefore avoids an immediate STA/LAN subnet collision.

## Build Evidence

The final release build was rebuilt cleanly with:

- `pio run -e wt32eth_release_final_safe -t clean; pio run -e wt32eth_release_final_safe`

Observed result:

- `SUCCESS`
- RAM usage: `98756 / 327680` (`30.1%`)
- Flash usage: `964589 / 1310720` (`73.6%`)

## Hardware Evidence

The final release build was erased, flashed, and booted on the local `WT32-ETH01` over `COM6` with:

- `pio run -e wt32eth_release_final_safe -t erase -t upload -t monitor --upload-port COM6 --monitor-port COM6`

Observed first-boot excerpt after erase:

- `stored_config_valid=no`
- `dhcp=on`
- `lan_config ip=10.11.13.221 mask=255.255.255.0`
- `recovery AP active reason=no_sta_config ip=192.168.4.1`
- `proxy_http ... listening=yes port=100`
- `proxy_vnc ... listening=yes port=5900`

Observed live runtime / UI evidence after the board rejoined the bench WiFi:

- `http://192.168.91.157/` responded with HTTP `200 OK`
- `http://192.168.91.157:100/` responded with HTTP `200 OK`
- TCP port `5900` accepted a live connection on `192.168.91.157`
- the status page reported `LAN + WiFi STA + AP`
- the status page reported `LAN active, WiFi STA connected, AP active`
- the status page reported `Bode services running`
- the status page reported `Time synced via WiFi STA`
- the status page reported the scope as reachable and both proxies as listening

Observed real save behavior through the final Web UI:

- the first explicit `POST /save` on unchanged Bode values persisted the temporary RAM recovery configuration into `NVS`
- a second explicit `POST /save` with the same values returned `No changes saved`
- an explicit `POST /save` changing `friendly_name` returned `Configuration saved`
- an explicit restore of the original `friendly_name` also returned `Configuration saved`
- a reboot after those tests reported `stored_config_valid=yes loaded_from_nvs=yes ram_recovery=no save_required=no reason=none`

## Coverage Limits In This Pass

- The live scope browser workflow on port `100` and the live noVNC workflow on port `5900` were not re-driven end-to-end in this exact final pass.
- Deliberate corruption of stored configuration was not forced on hardware in this pass because it would require destructive manipulation of `NVS`; that path was audited in code, while first boot on erased `NVS` was reproduced for real.
- `wt32eth_final_test_safe` was rebuilt successfully in this publish pass.
- `wt32eth_bringup_safe` was not fully revalidated in this publish pass because the local PlatformIO workspace hit a `.sconsign312.dblite` artifact error outside the release firmware path.

## Publish-Pass Notes

The publish-pass validation summary is reflected in:

- [../RELEASE_NOTES.md](../RELEASE_NOTES.md)
- the current release workflow and staging notes in [../release/README.md](../release/README.md)
