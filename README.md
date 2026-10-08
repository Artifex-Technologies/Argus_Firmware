# Argus Firmware

Custom [Flipper Zero](https://flipper.net) firmware, forked from the
[official flipperzero-firmware](https://github.com/flipperdevices/flipperzero-firmware).

> Fork of the official Flipper Zero firmware, licensed under the
> [GNU GPLv3](/LICENSE). All credit for the original platform, OS and
> application framework goes to [Flipper Devices](https://flipper.net).

## Part of the Chymaera ecosystem

| Component | Role |
| --- | --- |
| **Argus Firmware** | this repo — runs on the Flipper Zero |
| **Argus App** | desktop control app (qFlipper fork, Qt6) |
| **Chymaera OS** | Arch-based security distro hosting the app and Sarina |
| **Sarina** | local-model agent shipped in Chymaera Linux |

The device stays fully usable from **stock qFlipper** — that is a hard
requirement. Chymaera Zero App is the primary client and adds capability on
top, but the official flash and update paths must keep working.

## Goals

- **Instrumentation over novelty.** Get RF, NFC, 125 kHz RFID and BLE data
  off the device and into real tools — RF analyzers, Wireshark, and Sarina —
  rather than only viewing it on a 128×64 screen.
- **Capability in the firmware.** Features are built into the image rather
  than loaded as separate SD-card applications, so they are always present and
  can use internal APIs.
- **Stay current with upstream.** Flipper ships releases regularly; absorbing
  them must stay cheap. This is reconciled with the point above by *adding
  directories rather than editing upstream files* — see
  [CLAUDE.md](/CLAUDE.md).

### Hardware

Supported directly by the Flipper: sub-GHz (CC1101), NFC, 125 kHz RFID, BLE,
infrared, iButton, GPIO.

Via external hardware, using plumbing that already exists upstream:

- **WiFi** — no radio on the Flipper itself; an ESP32 devboard connects over
  GPIO UART through `applications/services/expansion`.
- **Extended sub-GHz range** — an external CC1101 module is supported by
  `applications/drivers/subghz/cc1101_ext`.

### Streaming

Telemetry is intended to reach Chymaera Linux over **Bluetooth**. The RPC
layer is transport-agnostic, so the same protobuf session runs over USB or
BLE. The BLE serial link tops out somewhere around **5–30 KB/s**, which is
the governing design constraint: the device emits decoded events and decimated
samples, never raw radio data.

Features are tiered accordingly — **USB-only** for bulk capture retrieval,
file sync and high-rate sampling; **either transport** for control, device
info, decoded events and telemetry. A USB-only feature refuses a Bluetooth
session outright rather than degrading silently.

## Status

Builds cleanly and installs through qFlipper. Verified on hardware:

```
firmware_origin_fork : Argus
firmware_version     : ARG-1.4.3r1
radio_alive          : true
enclave_valid        : true
```

Shipped so far:

- A Wii-style grid main menu, replacing the stock vertical list
- **BLE Toolkit** — a Bluetooth advertising app (main menu → BLE Toolkit):
  a plain identity beacon, two Apple Continuity pairing-popup modes for
  device-awareness testing, and an AirTag advertisement emulator. All
  advertising-only; this firmware's BLE stack has no scanning role, so
  detection-style features (e.g. AirTag *detection*) wait on external
  hardware — see [CLAUDE.md](/CLAUDE.md).

The RF/NFC/RFID instrumentation and Linux streaming described above are still
ahead, not shipped.

## Requirements

- Windows 10+ with PowerShell and Git (x86_64)
- macOS 12+ with Command Line tools (x86_64, arm64)
- Ubuntu 20.04+ with build-essential and Git (x86_64)

The Flipper Build Tool (`fbt`) downloads the ARM toolchain automatically on
first run. On Windows, make sure `git` is on `PATH` or the build fails with an
opaque error from inside SCons.

## Cloning

```shell
git clone --recursive https://github.com/Artifex-Technologies/Argus_Firmware.git
```

## Building

```shell
./fbt                                                  # debug firmware
./fbt COMPACT=1 DEBUG=0 updater_package                # release update package
```

Release artifacts land in `dist/f7-C/`, debug in `dist/f7-D/`.

## Installing

**Via the desktop app** — in qFlipper (or Chymaera Zero App), choose
*Install from file* and select
`dist/f7-C/flipper-z-f7-update-<suffix>.tgz`.

**Over USB from a shell** — close the desktop app first, or the serial port is
busy:

```shell
./fbt flash_usb        # firmware only (fast; use for iterating)
./fbt flash_usb_full   # includes resources/assets
```

**From the SD card** — copy `dist/f7-C/f7-update-<suffix>/` to `/ext/update/`
on the card and run it from the Flipper's file browser.

**With an in-circuit debugger** — `./fbt flash`.

## Verifying a Chymaera build

With the desktop app closed, over the serial console:

```
device_info
```

`firmware_origin_fork: Chymaera` distinguishes this from stock firmware.

## Staying in sync with upstream

`upstream` is configured fetch-only; this is a private fork and never pushes
to Flipper Devices' repositories.

```shell
git fetch upstream
git merge upstream/dev
```

## Documentation

- [CLAUDE.md](/CLAUDE.md) — architecture decisions, hardware constraints, and
  the pitfalls that cost real time
- [Flipper Build Tool](/documentation/fbt.md) — building, flashing, debugging
- [Applications](/documentation/AppsOnSDCard.md),
  [Application Manifest](/documentation/AppManifests.md) — developing apps
- [Hardware combos and Un-bricking](/documentation/KeyCombo.md) — recovery
- Upstream [Developer Documentation](https://developer.flipper.net/flipperzero/doxygen)
  still applies to most of the codebase

## Project structure

- `applications`        - Applications and services used in firmware
- `applications_user`   - Chymaera applications live here
- `assets`              - Assets used by applications and services
- `documentation`       - Documentation generation system configs and input files
- `furi`                - Furi Core: OS-level primitives and helpers
- `lib`                 - Our and 3rd party libraries, drivers, tools and etc...
- `site_scons`          - Build system configuration and modules
- `scripts`             - Supplementary scripts and various python libraries
- `targets`             - Firmware targets: platform specific code

## License

GNU GPLv3, inherited from upstream flipperzero-firmware. See [LICENSE](/LICENSE).