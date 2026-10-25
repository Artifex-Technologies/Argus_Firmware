/**
 * Host test for the Marauder scan-output parser. The lines below have the
 * exact shapes Marauder v1.19.2 prints, with made-up names and addresses.
 *
 *   cc -Wall -I.. test_marauder_proto.c ../marauder_proto.c -o t && ./t
 */
#include "marauder_proto.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                \
    do {                                                           \
        if(!(cond)) {                                              \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                            \
        }                                                          \
    } while(0)

int main(void) {
    MpLine l;
    const uint8_t bssid[6] = {0x62, 0x7f, 0xf0, 0x12, 0x34, 0x56};

    // Named, encrypted, with the CLI prompt in front (first line after the
    // "Scanning..." banner arrives that way).
    CHECK(mp_parse_line("> -78 Ch: 3 62:7f:f0:12:34:56 ESSID: Home WiFi 11 15 ", &l));
    CHECK(l.type == MpLineAp);
    CHECK(l.ap.rssi == -78);
    CHECK(l.ap.channel == 3);
    CHECK(memcmp(l.ap.bssid, bssid, 6) == 0);
    CHECK(strcmp(l.ap.ssid, "Home WiFi") == 0);
    CHECK(!l.ap.hidden);
    CHECK(l.ap.encrypted);

    // Hidden: empty SSID, then Marauder prints the BSSID in its place.
    CHECK(mp_parse_line("-79 Ch: 11 62:7f:f0:12:34:56 ESSID:  62:7f:f0:12:34:56 11 15 ", &l));
    CHECK(l.type == MpLineAp);
    CHECK(l.ap.hidden);
    CHECK(l.ap.ssid[0] == 0);
    CHECK(l.ap.channel == 11);

    // Open network: Privacy bit (0x10 in the low byte) clear.
    CHECK(mp_parse_line("-50 Ch: 6 aa:bb:cc:dd:ee:01 ESSID: Cafe Guest 01 04 ", &l));
    CHECK(l.type == MpLineAp);
    CHECK(!l.ap.encrypted);
    CHECK(strcmp(l.ap.ssid, "Cafe Guest") == 0);

    // An SSID that itself ends in a space keeps it; one with digits that
    // look like hex keeps them.
    CHECK(mp_parse_line("-60 Ch: 1 aa:bb:cc:dd:ee:02 ESSID: Net  11 15", &l));
    CHECK(strcmp(l.ap.ssid, "Net ") == 0);
    CHECK(mp_parse_line("-60 Ch: 1 aa:bb:cc:dd:ee:03 ESSID: Room 12 31 14 ", &l));
    CHECK(strcmp(l.ap.ssid, "Room 12") == 0);
    CHECK(l.ap.encrypted);

    // Over-long SSIDs are clipped, never overflow.
    CHECK(mp_parse_line(
        "-60 Ch: 1 aa:bb:cc:dd:ee:04 ESSID: 0123456789012345678901234567890123456789 11 15", &l));
    CHECK(strlen(l.ap.ssid) == MP_SSID_MAX);

    // Station lines, both directions, normalised to station -> AP.
    const uint8_t sta[6] = {0xcc, 0xdb, 0xa7, 0x12, 0x34, 0x56};
    const uint8_t ap[6] = {0x88, 0xfe, 0xb6, 0x12, 0x34, 0x56};
    CHECK(mp_parse_line("1: sta: cc:db:a7:12:34:56 -> ap: 88:fe:b6:12:34:56", &l));
    CHECK(l.type == MpLineStation);
    CHECK(memcmp(l.station.station, sta, 6) == 0 && memcmp(l.station.ap, ap, 6) == 0);
    CHECK(mp_parse_line("2: ap: 88:fe:b6:12:34:56 -> sta: cc:db:a7:12:34:56", &l));
    CHECK(memcmp(l.station.station, sta, 6) == 0 && memcmp(l.station.ap, ap, 6) == 0);

    // IPv6 multicast "station" is a group address, not a device.
    const uint8_t mcast[6] = {0x33, 0x33, 0x00, 0x00, 0x00, 0x01};
    CHECK(mp_mac_is_group(mcast));
    CHECK(!mp_mac_is_group(sta));

    // Banner lines.
    CHECK(mp_parse_line("Version: v1.19.2\r", &l));
    CHECK(l.type == MpLineVersion && strcmp(l.text, "v1.19.2") == 0);
    CHECK(mp_parse_line("Firmware: Marauder", &l));
    CHECK(l.type == MpLineFirmware && strcmp(l.text, "Marauder") == 0);

    // Everything else is ignored, not misparsed.
    const char* junk[] = {
        "#scanall",
        "Scanning for APs and Stations. Stop with stopscan",
        "> ",
        "",
        "-78 Ch: 3 62:7f:f0:12:34:56 ESSID: truncated",
        "-78 Ch: 3 62:7f:f0:12:34 ESSID: Short mac 11 15",
        "12 Ch: 3 62:7f:f0:12:34:56 ESSID: positive rssi 11 15",
        "[0][CH:3] Home WiFi -79",
        "1: sta: cc:db:a7:12:34:56 -> sta: 88:fe:b6:12:34:56",
    };
    for(size_t i = 0; i < sizeof(junk) / sizeof(junk[0]); i++) {
        CHECK(!mp_parse_line(junk[i], &l));
        CHECK(l.type == MpLineNone);
    }

    printf("%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
