# Release Artifacts

The `release/` directory is a local staging area produced by `scripts/build_bins.py`.

Generated binaries placed here are ignored by Git and are intended to be uploaded as GitHub Release assets, not committed into the repository history.

## Primary Runtime

- `wt32eth_release_final_safe/app.bin`

Supporting files for clean manual flashing:

- `wt32eth_release_final_safe/bootloader.bin`
- `wt32eth_release_final_safe/partitions.bin`

## Auxiliary Builds

- Auxiliary builds can still be generated locally from source when needed, but the public release flow centers on the primary runtime build above.

## Which File To Use

- Use `wt32eth_release_final_safe/app.bin` for normal updates when the target already has a compatible bootloader and partition table.
- Use the primary build's `bootloader.bin`, `partitions.bin`, and `app.bin` together for a clean manual flash.
- For published binaries, prefer the attached GitHub Release assets so users get unique filenames and a coherent checksum set.

## Checksums

`SHA256SUMS.txt` contains SHA-256 hashes for the locally staged artifacts that are intended for the corresponding GitHub Release upload.

## Final Runtime Policy

- the final release keeps normal `Preferences` / `NVS` writes user-driven
- identical saves are treated as no-op saves and do not rewrite flash
- invalid, incomplete, or incompatible stored configuration is repaired only in RAM until the user explicitly saves it
- boot-time automatic writes are no longer used for migration, first boot, config repair, or recovery fallback

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
