/**
 * Host test for the LD2450 protocol driver.
 *
 *   cc -Wall -I.. test_ld2450.c ../ld2450.c -o test_ld2450 && ./test_ld2450
 */
#include "ld2450.h"

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

static Ld2450Event feed_all(Ld2450Parser* p, const uint8_t* data, size_t len, int* events) {
    Ld2450Event last = Ld2450EventNone;
    for(size_t i = 0; i < len; i++) {
        Ld2450Event e = ld2450_parser_feed(p, data[i]);
        if(e != Ld2450EventNone) {
            last = e;
            if(events) (*events)++;
        }
    }
    return last;
}

int main(void) {
    Ld2450Parser p;
    ld2450_parser_reset(&p);

    // The worked example from the HLK serial protocol manual: one target at
    // x = -782 mm, y = 1713 mm, speed -16 cm/s, resolution 320 mm.
    const uint8_t manual[] = {0xAA, 0xFF, 0x03, 0x00, 0x0E, 0x03, 0xB1, 0x86, 0x10, 0x00,
                              0x40, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                              0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x55, 0xCC};
    CHECK(feed_all(&p, manual, sizeof(manual), NULL) == Ld2450EventFrame);
    CHECK(p.frame.target[0].present);
    CHECK(p.frame.target[0].x_mm == -782);
    CHECK(p.frame.target[0].y_mm == 1713);
    CHECK(p.frame.target[0].speed_cm_s == -16);
    CHECK(p.frame.target[0].resolution_mm == 320);
    CHECK(!p.frame.target[1].present);
    CHECK(!p.frame.target[2].present);

    // Encoder round-trips, including positive x, zero x, and positive speed.
    Ld2450Frame f = {0};
    f.target[0] = (Ld2450Target){true, 1500, 3000, 25, 360};
    f.target[1] = (Ld2450Target){true, 0, 800, -100, 360};
    f.target[2] = (Ld2450Target){false, 0, 0, 0, 0};
    uint8_t enc[LD2450_FRAME_SIZE];
    ld2450_encode_frame(enc, &f);
    CHECK(feed_all(&p, enc, sizeof(enc), NULL) == Ld2450EventFrame);
    CHECK(memcmp(&p.frame.target[0], &f.target[0], sizeof(Ld2450Target)) == 0);
    CHECK(memcmp(&p.frame.target[1], &f.target[1], sizeof(Ld2450Target)) == 0);
    CHECK(!p.frame.target[2].present);

    // Resync: garbage, a truncated frame, then a good one. Exactly one frame.
    uint8_t noisy[64 + LD2450_FRAME_SIZE];
    size_t n = 0;
    const uint8_t junk[] = {0x00, 0xAA, 0x13, 0xFD, 0xFC, 0x00, 0xAA, 0xFF, 0x03};
    memcpy(noisy + n, junk, sizeof(junk));
    n += sizeof(junk);
    memcpy(noisy + n, enc, sizeof(enc));
    n += sizeof(enc);
    int events = 0;
    ld2450_parser_reset(&p);
    CHECK(feed_all(&p, noisy, n, &events) == Ld2450EventFrame);
    CHECK(events == 1);
    CHECK(p.frame.target[0].x_mm == 1500);

    // A frame with a corrupt footer is dropped, and the next one still parses.
    uint8_t bad[LD2450_FRAME_SIZE];
    memcpy(bad, enc, sizeof(bad));
    bad[LD2450_FRAME_SIZE - 1] = 0x00;
    events = 0;
    feed_all(&p, bad, sizeof(bad), &events);
    CHECK(events == 0);
    CHECK(feed_all(&p, enc, sizeof(enc), NULL) == Ld2450EventFrame);

    // Command builder: enable config, per the manual.
    uint8_t cmd[32];
    const uint8_t one[] = {0x01, 0x00};
    size_t len = ld2450_build_command(cmd, sizeof(cmd), LD2450_CMD_ENABLE_CONFIG, one, 2);
    const uint8_t expect_enable[] = {
        0xFD, 0xFC, 0xFB, 0xFA, 0x04, 0x00, 0xFF, 0x00, 0x01, 0x00, 0x04, 0x03, 0x02, 0x01};
    CHECK(len == sizeof(expect_enable));
    CHECK(memcmp(cmd, expect_enable, len) == 0);
    CHECK(ld2450_build_command(cmd, 8, LD2450_CMD_END_CONFIG, NULL, 0) == 0);

    // ACK for enable config: status 0, protocol version 1, buffer 0x40.
    const uint8_t ack_enable[] = {
        0xFD,
        0xFC,
        0xFB,
        0xFA,
        0x08,
        0x00,
        0xFF,
        0x01,
        0x00,
        0x00,
        0x01,
        0x00,
        0x40,
        0x00,
        0x04,
        0x03,
        0x02,
        0x01};
    CHECK(feed_all(&p, ack_enable, sizeof(ack_enable), NULL) == Ld2450EventAck);
    CHECK(ld2450_ack_ok(&p.ack, LD2450_CMD_ENABLE_CONFIG));
    CHECK(!ld2450_ack_ok(&p.ack, LD2450_CMD_END_CONFIG));
    CHECK(p.ack.data_len == 4);

    // Failed ACK.
    uint8_t ack_fail[sizeof(ack_enable)];
    memcpy(ack_fail, ack_enable, sizeof(ack_fail));
    ack_fail[8] = 0x01;
    CHECK(feed_all(&p, ack_fail, sizeof(ack_fail), NULL) == Ld2450EventAck);
    CHECK(!ld2450_ack_ok(&p.ack, LD2450_CMD_ENABLE_CONFIG));

    // Firmware version ACK: type 0x0000, major 0x0102, minor 0x22062416.
    const uint8_t ack_ver[] = {0xFD, 0xFC, 0xFB, 0xFA, 0x0C, 0x00, 0xA0, 0x01, 0x00, 0x00, 0x00,
                               0x00, 0x02, 0x01, 0x16, 0x24, 0x06, 0x22, 0x04, 0x03, 0x02, 0x01};
    CHECK(feed_all(&p, ack_ver, sizeof(ack_ver), NULL) == Ld2450EventAck);
    char ver[24];
    CHECK(ld2450_ack_version(&p.ack, ver, sizeof(ver)));
    CHECK(strcmp(ver, "V1.02.22062416") == 0);

    // A data frame immediately after an ACK, as happens when config ends.
    uint8_t mixed[sizeof(ack_enable) + LD2450_FRAME_SIZE];
    memcpy(mixed, ack_enable, sizeof(ack_enable));
    memcpy(mixed + sizeof(ack_enable), enc, sizeof(enc));
    events = 0;
    CHECK(feed_all(&p, mixed, sizeof(mixed), &events) == Ld2450EventFrame);
    CHECK(events == 2);

    // Oversized length field must not overrun the buffer.
    const uint8_t huge[] = {0xFD, 0xFC, 0xFB, 0xFA, 0xFF, 0x7F};
    feed_all(&p, huge, sizeof(huge), NULL);
    CHECK(p.len == 0);

    printf("%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
