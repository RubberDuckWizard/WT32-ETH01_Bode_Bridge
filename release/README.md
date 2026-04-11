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

## Flashing Reference

See [../FLASHING.md](../FLASHING.md) for exact flashing commands and boot/recovery notes.
