# Flashing

## Serial Parameters

- Default serial speed for logs: `115200`
- Replace `COMx` in the examples below with the correct serial port for your machine

## Preferred Source-Based Upload

If you are building from source, use PlatformIO:

```bash
pio run -e wt32eth_release_final_safe -t upload --upload-port COMx
```

Monitor the serial output:

```bash
pio device monitor -b 115200 -p COMx
```

## Precompiled Application Update

Use this when the module already has a compatible bootloader and partition layout from this project or from a previous PlatformIO flash of the same board profile:

```bash
esptool.py --chip esp32 --port COMx --baud 460800 write_flash 0x10000 release/wt32eth_release_final_safe/app.bin
```

This is the least invasive precompiled update path and is the recommended way to update an already prepared board.

## Clean Manual Flash

Use this when the board is blank, the existing flash layout is unknown, or you want to restore the standard bootloader and partition layout used by the project:

```bash
esptool.py --chip esp32 --port COMx --baud 460800 write_flash ^
  0x1000 release/wt32eth_release_final_safe/bootloader.bin ^
  0x8000 release/wt32eth_release_final_safe/partitions.bin ^
  0x10000 release/wt32eth_release_final_safe/app.bin
```

PowerShell one-line equivalent:

```powershell
esptool.py --chip esp32 --port COMx --baud 460800 write_flash 0x1000 release/wt32eth_release_final_safe/bootloader.bin 0x8000 release/wt32eth_release_final_safe/partitions.bin 0x10000 release/wt32eth_release_final_safe/app.bin
```

## Auxiliary Build Flashing

Recovery build:

```bash
esptool.py --chip esp32 --port COMx --baud 460800 write_flash 0x10000 release/wt32eth_bringup_safe/app.bin
```

FY6900 service build:

```bash
esptool.py --chip esp32 --port COMx --baud 460800 write_flash 0x10000 release/wt32eth_final_test_safe/app.bin
```

## Boot And Recovery Notes

- If automatic bootloader entry does not work on your board, hold the module in boot mode and trigger reset manually during flashing.
- The main release build keeps a recovery AP available for configuration access.
- The `wt32eth_bringup_safe` build is the fastest fallback when stored configuration or normal runtime behavior prevents access.
- The `wt32eth_final_test_safe` build is intended for manual FY6900 diagnostics and should not be treated as the normal end-user runtime.
