#include "ble_toolkit_payload.h"

#include <furi_hal_random.h>
#include <string.h>

// Apple's Bluetooth SIG company identifier (little-endian on air: 4C 00).
// Continuity type/length values below come from the public reverse-
// engineering writeups at https://github.com/furiousMAC/continuity/
// (Nearby Action) and https://github.com/ECTO-1A/AppleJuice/ (Proximity
// Pairing) - the same references cited by every open-source BLE beacon
// tool that implements these. Only the two non-destructive, well-documented
// message types are implemented: Proximity Pairing (the AirPods-style "new
// device" popup) and Nearby Action (the AppleTV/HomeKit-style setup popup).
// A separate, undocumented advertisement shape that reliably crashes iOS
// exists in the wild; it is deliberately not implemented here.
#define APPLE_COMPANY_ID_LO 0x4C
#define APPLE_COMPANY_ID_HI 0x00

#define CONTINUITY_TYPE_PROXIMITY_PAIR 0x07
#define CONTINUITY_TYPE_NEARBY_ACTION 0x0F

const char* const ble_toolkit_mode_names[BleToolkitModeCount] = {
    [BleToolkitModeBeacon] = "Argus Beacon",
    [BleToolkitModeProximityPair] = "Proximity Pair Popup",
    [BleToolkitModeNearbyAction] = "Nearby Action Popup",
    [BleToolkitModeAirTag] = "AirTag Emulator",
};

bool ble_toolkit_mode_should_cycle(BleToolkitMode mode) {
    return mode != BleToolkitModeBeacon;
}

typedef struct {
    uint16_t model;
    uint8_t color;
} ProximityPairPreset;

// A small curated set rather than the full device catalog: enough variety
// to look like real nearby accessories without a config UI. Model/prefix
// values from the AppleJuice reference above.
static const ProximityPairPreset pp_presets[] = {
    {0x0220, 0x00}, // AirPods, White
    {0x0E20, 0x00}, // AirPods Pro, White
    {0x0A20, 0x00}, // AirPods Max, White
    {0x0A20, 0x02}, // AirPods Max, Red
};
#define PP_PRESETS_COUNT (sizeof(pp_presets) / sizeof(pp_presets[0]))

#define AIRTAG_MODEL 0x0055
#define AIRTAG_PREFIX_NEW 0x05
#define PP_PREFIX_NEW_DEVICE 0x07

static uint8_t
    build_proximity_pair(uint8_t* data, uint16_t model, uint8_t color, uint8_t prefix) {
    // 6-byte header (Length, AD Type, Company ID x2, Continuity Type,
    // Continuity Length) plus a 25-byte Proximity Pairing payload.
    const uint8_t continuity_len = 25;
    const uint8_t total_len = 6 + continuity_len;

    uint8_t i = 0;
    data[i++] = total_len - 1; // AD Length field excludes itself
    data[i++] = 0xFF; // Manufacturer Specific
    data[i++] = APPLE_COMPANY_ID_LO;
    data[i++] = APPLE_COMPANY_ID_HI;
    data[i++] = CONTINUITY_TYPE_PROXIMITY_PAIR;
    data[i++] = continuity_len;

    data[i++] = prefix;
    data[i++] = (model >> 8) & 0xFF;
    data[i++] = model & 0xFF;
    data[i++] = 0x55; // Status byte, observed constant across samples
    data[i++] = ((furi_hal_random_get() % 10) << 4) | (furi_hal_random_get() % 10); // Battery
    data[i++] = ((furi_hal_random_get() % 8) << 4) | (furi_hal_random_get() % 10); // Case/charge
    data[i++] = furi_hal_random_get() & 0xFF; // Lid open counter
    data[i++] = color;
    data[i++] = 0x00;
    furi_hal_random_fill_buf(&data[i], 16); // Encrypted payload placeholder
    i += 16;

    return total_len;
}

static uint8_t build_nearby_action(uint8_t* data, uint8_t action, uint8_t flags) {
    // 6-byte header plus a 5-byte Nearby Action payload.
    const uint8_t continuity_len = 5;
    const uint8_t total_len = 6 + continuity_len;

    uint8_t i = 0;
    data[i++] = total_len - 1;
    data[i++] = 0xFF;
    data[i++] = APPLE_COMPANY_ID_LO;
    data[i++] = APPLE_COMPANY_ID_HI;
    data[i++] = CONTINUITY_TYPE_NEARBY_ACTION;
    data[i++] = continuity_len;

    data[i++] = flags;
    data[i++] = action;
    furi_hal_random_fill_buf(&data[i], 3); // Authentication tag placeholder
    i += 3;

    return total_len;
}

static uint8_t build_beacon(uint8_t* data) {
    // Not an Apple/Google/etc. protocol: company ID 0xFFFF is reserved by
    // the Bluetooth SIG for manufacturer testing, which is the correct
    // choice for a beacon that isn't emulating a real vendor's device.
    static const char payload[] = "ARGUS";
    const uint8_t payload_len = sizeof(payload) - 1; // exclude NUL
    const uint8_t total_len = 4 + payload_len; // Length + Type + CompanyID(2) + payload

    uint8_t i = 0;
    data[i++] = total_len - 1;
    data[i++] = 0xFF;
    data[i++] = 0xFF; // Company ID low (reserved for testing)
    data[i++] = 0xFF; // Company ID high
    memcpy(&data[i], payload, payload_len);
    i += payload_len;

    return total_len;
}

uint8_t ble_toolkit_payload_build(BleToolkitMode mode, uint8_t* data) {
    switch(mode) {
    case BleToolkitModeProximityPair: {
        const ProximityPairPreset* preset = &pp_presets[furi_hal_random_get() % PP_PRESETS_COUNT];
        return build_proximity_pair(data, preset->model, preset->color, PP_PREFIX_NEW_DEVICE);
    }
    case BleToolkitModeNearbyAction: {
        // A small set of documented, non-destructive action codes.
        static const uint8_t actions[] = {0x09, 0x0B, 0x06}; // Setup New iPhone/HomePod/AppleTV
        uint8_t action = actions[furi_hal_random_get() % (sizeof(actions) / sizeof(actions[0]))];
        return build_nearby_action(data, action, 0xC0);
    }
    case BleToolkitModeAirTag:
        return build_proximity_pair(data, AIRTAG_MODEL, 0x00, AIRTAG_PREFIX_NEW);
    case BleToolkitModeBeacon:
    default:
        return build_beacon(data);
    }
}
