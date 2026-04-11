# Notice And Third-Party Licenses

## Project License

The original source files in this repository are released under the MIT License. See [LICENSE](LICENSE).

## Build And Runtime Dependencies

This project is built with external tooling and frameworks that are not copied into this repository. Typical dependencies include:

- PlatformIO Core
- PlatformIO Espressif32 platform packages
- Arduino-ESP32 framework packages
- Toolchain, flashing, and supporting libraries pulled by PlatformIO for the selected board and framework

Those components remain under their own licenses and copyright notices.

## Generated Firmware Binaries

The precompiled binaries in `release/` are generated from this project's source code together with the external framework and toolchain packages listed above. Anyone redistributing those binaries should review the licenses that accompany the installed PlatformIO packages used for the build.

## Vendor Reference Material

Local vendor reference files and engineering notes kept outside the public Git history are not part of the repository license grant unless their upstream owners explicitly permit redistribution.
