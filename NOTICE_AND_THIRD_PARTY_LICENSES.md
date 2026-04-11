# Notice and Third-Party Licenses

## Project License

Unless noted otherwise, the original source files in this repository are released under the MIT License.
See `LICENSE` for the full project license text.

## Scope of This Notice

This repository primarily contains original project source code, documentation, and release artifacts for the
WT32-ETH01 Bode Bridge firmware.

The project is built with PlatformIO using the `espressif32` platform and the Arduino framework for ESP32.
The repository does not appear to vendor separate third-party library source trees in its own top-level content;
instead, the main third-party items are the external build/runtime frameworks and packages used to compile the
firmware.

## Third-Party Components Acknowledged

### 1. PlatformIO Core
- Upstream project: PlatformIO Core
- Upstream repository: https://github.com/platformio/platformio-core
- License: Apache License 2.0
- Notes: Used as the build system / package manager / CLI environment.

### 2. PlatformIO Espressif32 Platform
- Upstream project: PlatformIO Espressif32 platform
- Upstream repository: https://github.com/platformio/platform-espressif32
- License: Apache License 2.0
- Notes: Used to provide board support, builder scripts, packages, and integration for ESP32 targets.

### 3. Arduino core for ESP32 (Arduino-ESP32)
- Upstream project: Arduino core for ESP32
- Upstream repository: https://github.com/espressif/arduino-esp32
- License: LGPL-2.1-or-later
- Notes: This project is configured with `framework = arduino`, and source files use framework headers such as
  `Arduino.h`, `WiFi.h`, and `WebServer.h`.

### 4. ESP-IDF (Espressif IoT Development Framework)
- Upstream project: ESP-IDF
- Upstream documentation: https://docs.espressif.com/projects/esp-idf/en/stable/esp32/COPYRIGHT.html
- Primary license for original Espressif source: Apache License 2.0
- Important note: ESP-IDF also includes additional third-party components under their own licenses. Espressif's
  published copyright/licensing page documents examples including BSD-, MIT-, and Apache-licensed components.
  Where upstream component headers or upstream notices provide more specific terms, those upstream terms take
  precedence.

## Prebuilt Firmware Artifacts

This repository includes prebuilt firmware artifacts under `release/`.
Those binaries are expected to contain object code produced from the project sources together with the selected
framework/toolchain packages used at build time.
Accordingly, redistribution of the binaries may also involve the licenses of the relevant upstream framework and
runtime components listed above.

## No Separate Third-Party Source Bundles Intentionally Republished Here

At the repository level, third-party frameworks and packages are referenced through the PlatformIO build
configuration rather than copied in as independent source trees.
If future revisions vendor additional third-party code directly into this repository, this notice should be updated
to add the relevant package names, authors, licenses, and required attribution text.

## Upstream Licenses and Notices Control

This file is an informational summary only.
For exact legal terms, consult:
- the `LICENSE` file in this repository;
- the license files included with the relevant upstream packages;
- the upstream copyright/license pages maintained by PlatformIO, Espressif, and other component authors.

If there is any conflict between this summary and an upstream component's own license text or source-file header,
the upstream license text / source-file header controls for that component.