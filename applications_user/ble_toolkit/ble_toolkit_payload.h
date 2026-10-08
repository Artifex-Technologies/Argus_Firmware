/**
 * @file ble_toolkit_payload.h
 * BLE advertising payload construction for BLE Toolkit.
 *
 * All modes here build a single BLE advertising structure and hand it to
 * the platform's extra-beacon API (furi_hal_bt_extra_beacon_*); nothing in
 * this file touches the radio or GAP state directly.
 */
#pragma once

#include <stdint.h>
#include <extra_beacon.h>

typedef enum {
    BleToolkitModeBeacon, /**< Plain Argus identity beacon */
    BleToolkitModeProximityPair, /**< Apple "new device" pairing popup */
    BleToolkitModeNearbyAction, /**< Apple "nearby action" popup (e.g. AppleTV setup) */
    BleToolkitModeAirTag, /**< Apple AirTag advertisement emulation */
    BleToolkitModeCount,
} BleToolkitMode;

extern const char* const ble_toolkit_mode_names[BleToolkitModeCount];

/** True if `mode` benefits from being rebuilt periodically while active.
 * Real devices vary their advertisement fields between transmissions; a
 * fixed payload is easy for a host to filter out or ignore. Only used for
 * a UI hint, not required for correctness. */
bool ble_toolkit_mode_should_cycle(BleToolkitMode mode);

/** Build the advertising payload for `mode` into `data`.
 *
 * @param      mode  Which payload to build
 * @param      data  Output buffer, at least EXTRA_BEACON_MAX_DATA_SIZE bytes
 * @return     Number of bytes written
 */
uint8_t ble_toolkit_payload_build(BleToolkitMode mode, uint8_t* data);
