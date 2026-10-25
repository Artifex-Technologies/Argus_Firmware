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
| **Sarina** | local-model agent shipped in Chymaera OS |

The device stays fully usable from **stock qFlipper** — that is a hard
requirement. Argus App is the primary client and adds capability on
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

Via external hardware on the GPIO header:

- **Wi-Fi**: the Flipper has no Wi-Fi radio. The official Wi-Fi devboard
  (ESP32-S2) running [ESP32 Marauder](https://github.com/justcallmekoko/ESP32Marauder)
  connects over UART (pins 13/14).
- **mmWave radar**: an HLK-LD2450 24 GHz sensor, over UART (pins 13/14 or
  15/16).
- **Extended sub-GHz range**: an external CC1101 module is supported by
  `applications/drivers/subghz/cc1101_ext` and selectable at runtime.

### Streaming

Telemetry is intended to reach Chymaera OS over **Bluetooth**. The RPC
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
firmware.version     : ARG-1.4.3r1+8     (development build; see "Version numbers")
firmware.origin.fork : Argus
firmware.api         : 88.2      (same as upstream, so official catalog apps install)
radio.alive          : true
enclave.valid        : true
```

### What's built in

| Where | App | What it does |
| --- | --- | --- |
| Main menu | **Wii-style grid menu** | 3×2 tile grid, replacing the stock vertical list |
| Main menu | **BLE Kit** | Bluetooth advertising: an identity beacon, two Apple Continuity pairing-popup modes for device-awareness testing, and an AirTag advertisement emulator with trigger sounds. Advertising only, because this BLE stack has no scanning role. |
| Main menu | **RSSI** | Live sub-GHz signal meter with peak hold and a scrolling graph. Switches between the internal and an external CC1101, and logs CSV to `/ext/rf_monitor/`. |
| Main menu | **MFKey** | Recovers MIFARE Classic keys from nonces captured by NFC → Detect Reader. Recovered keys go to the user dictionary. |
| Main menu | **Radar** | Display for an HLK-LD2450 mmWave sensor: up to 3 people as dots with trails, a target list, and 3 m / 6 m zoom. Has a demo mode that runs without the sensor, and logs CSV to `/ext/ld2450/`. |
| GPIO → | **Wi-Fi Scanner (ESP32)** | Lists nearby networks through the Marauder devboard: signal, channel, encrypted or open, and connected devices. Passive. Logs CSV to `/ext/wifi_scan/`. |

The NFC system dictionary is also extended to 4,479 MIFARE Classic keys
(the union of Unleashed's and Momentum's), so more cards dump completely and
emulate.

Not shipped yet: the live Bluetooth streaming to Chymaera OS described above.
Today the Argus App pulls the apps' CSV capture files off the SD card over the
standard RPC storage link instead.

### Hardware notes

- **Wi-Fi devboard**: flash it with Marauder's *Flipper Zero WiFi Dev Board*
  build. Marauder v1.19 removed the `scanap` command, so older companion
  apps show an empty scan. The built-in Wi-Fi Scanner uses `scanall`.
- **LD2450**: wire 5V to pin 1, GND to pin 8, and TX/RX crossed to 13/14 or
  15/16. Choose the pins in Radar → Settings. The sensor ships with its own
  Bluetooth switched on, and Radar can turn it off.

## Requirements

- Linux (x86_64) with Git, which is the primary development platform
- macOS 12+ with Command Line tools (x86_64, arm64)
- Windows 10+ with PowerShell and Git (x86_64)

The Flipper Build Tool (`fbt`) downloads the ARM toolchain automatically on
first run. `git` must be on `PATH`, because the build stamps its version from
git. On Windows a missing git shows up as an opaque `FileNotFoundError` from
inside SCons.

On Linux, your user needs access to the Flipper's serial port. Install the
udev rules from Argus App (`setup_rules.sh`).

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

**Via the desktop app** — in qFlipper (or Argus App), choose
*Install from file* and select
`dist/f7-C/flipper-z-f7-update-<suffix>.tgz`.

**Over USB from a shell**: close the desktop app first, or the serial port is
busy.

```shell
./fbt COMPACT=1 DEBUG=0 flash_usb_full   # firmware + SD resources (recommended)
./fbt flash_usb                          # firmware only (faster, for iterating)
```

Prefer `flash_usb_full`. Some apps live on the SD card rather than in the
firmware image. The stock GPIO app is one of them, and that is where the Wi-Fi
Scanner entry is. A firmware-only flash leaves those apps at their old version.

**Your dolphin's level, XP and settings survive updates.** The updater backs
up internal storage before flashing and restores it afterwards. On current
firmware that storage also lives on the SD card, which a flash does not
touch. That holds for every path here except replacing or wiping the SD
card.

**From the SD card** — copy `dist/f7-C/f7-update-<suffix>/` to `/ext/update/`
on the card and run it from the Flipper's file browser.

**With an in-circuit debugger** — `./fbt flash`.

## Verifying an Argus build

With the desktop app closed, over the serial console:

```
info device
```

`firmware.origin.fork: Argus` distinguishes this from stock firmware.

### Version numbers

`ARG-1.4.3r1` reads as **ARG**, then the upstream Flipper release this is
based on (**1.4.3**), then our revision of it (**r1**). A development build
adds `+N` for the commits since the last `ARG-*r*` tag. A new upstream release
restarts at `r1` (`ARG-1.4.4r1`). The exact commit is reported separately as
`firmware.commit.hash`.

## Staying in sync with upstream

`upstream` is configured fetch-only, so this fork never pushes to Flipper
Devices' repositories.

```shell
git remote add upstream https://github.com/flipperdevices/flipperzero-firmware.git
git remote set-url --push upstream no_push
git fetch upstream
git merge upstream/dev
```

This repository's own history was restarted. Upstream's fork point
(`7f0b6e1c`) is recorded as an ancestor, so merges are ordinary three-way
merges. Use `git log --first-parent` to see only Argus commits. After merging
a new upstream release, update `UPSTREAM_VERSION` in `scripts/version.py` by
hand.

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
- `applications_user`   - Argus applications live here (`ble_toolkit`, `rf_monitor`, `mfkey`, `ld2450_radar`, `wifi_scanner`)
- `assets`              - Assets used by applications and services
- `documentation`       - Documentation generation system configs and input files
- `furi`                - Furi Core: OS-level primitives and helpers
- `lib`                 - Our and 3rd party libraries, drivers, tools and etc...
- `site_scons`          - Build system configuration and modules
- `scripts`             - Supplementary scripts and various python libraries
- `targets`             - Firmware targets: platform specific code

## License

GNU GPLv3, inherited from upstream flipperzero-firmware. See [LICENSE](/LICENSE).
