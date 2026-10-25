/**
 * @file marauder_proto.h
 * Parser for the scan output of ESP32 Marauder's serial CLI.
 *
 * Pure C with no Furi dependencies, host-tested in test/test_marauder_proto.c.
 *
 * Marauder's CLI is meant for people, not machines, so this parses the
 * exact line shapes printed by `scanall` and `info` (checked against
 * Marauder v1.19.2, WiFiScan.cpp). Lines may carry a leading "> " prompt.
 *
 *   -78 Ch: 3 62:7f:f0:12:34:56 ESSID: Home WiFi 11 15
 *   -79 Ch: 3 62:7f:f0:12:34:56 ESSID:  62:7f:f0:12:34:56 11 15   (hidden)
 *   1: sta: cc:db:a7:12:34:56 -> ap: 88:fe:b6:12:34:56
 *   2: ap: 88:fe:b6:12:34:56 -> sta: cc:db:a7:12:34:56
 *   Version: v1.19.2
 *
 * The two trailing hex bytes on an AP line are the beacon's capability
 * field, low byte first; its Privacy bit says whether the network is
 * encrypted. Marauder does not print the cipher suite, so WPA2 vs WPA3 is
 * not knowable from this output.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define MP_SSID_MAX 32

typedef enum {
    MpLineNone,
    MpLineAp,
    MpLineStation, // a station seen talking to an AP
    MpLineVersion, // "Version: v1.19.2"
    MpLineFirmware, // "Firmware: Marauder"
} MpLineType;

typedef struct {
    int8_t rssi;
    uint8_t channel;
    uint8_t bssid[6];
    char ssid[MP_SSID_MAX + 1]; // empty when hidden
    bool hidden;
    bool encrypted;
} MpAp;

typedef struct {
    uint8_t station[6];
    uint8_t ap[6];
} MpStation;

typedef struct {
    MpLineType type;
    MpAp ap;
    MpStation station;
    char text[24]; // version or firmware name
} MpLine;

/** Parse one line, without its newline. Returns false for unrecognised lines. */
bool mp_parse_line(const char* line, MpLine* out);

/** Parse "aa:bb:cc:dd:ee:ff". Returns false on anything else. */
bool mp_parse_mac(const char* s, uint8_t out[6]);

/** True for multicast/broadcast addresses, which are not real stations. */
bool mp_mac_is_group(const uint8_t mac[6]);
