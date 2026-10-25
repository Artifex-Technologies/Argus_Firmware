/**
 * @file ld2450.h
 * HLK-LD2450 24 GHz mmWave radar: UART protocol driver.
 *
 * Pure C with no Furi dependencies, so it can be host-tested
 * (test/test_ld2450.c) and reused by any app that talks to the sensor.
 *
 * The sensor streams a 30-byte data frame ~10 times a second reporting up
 * to three tracked targets, and accepts command frames that must be wrapped
 * in an enable-config / end-config pair. Both frame types share one byte
 * stream, so ld2450_parser_feed() recognises either.
 *
 * Protocol per the HLK-LD2450 serial protocol manual, cross-checked against
 * ESPHome's ld2450 component.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LD2450_DEFAULT_BAUD 256000
#define LD2450_MAX_TARGETS  3
#define LD2450_FRAME_SIZE   30
#define LD2450_MAX_ACK      64
#define LD2450_MAX_RANGE_MM 6000

// Command words. ACKs echo them with LD2450_ACK_BIT set.
#define LD2450_CMD_ENABLE_CONFIG 0x00FF // value 0x0001
#define LD2450_CMD_END_CONFIG    0x00FE
#define LD2450_CMD_SINGLE_TARGET 0x0080
#define LD2450_CMD_MULTI_TARGET  0x0090
#define LD2450_CMD_QUERY_VERSION 0x00A0
#define LD2450_CMD_FACTORY_RESET 0x00A2
#define LD2450_CMD_RESTART       0x00A3
#define LD2450_CMD_BLUETOOTH     0x00A4 // value 0x0001 on, 0x0000 off; needs restart
#define LD2450_CMD_QUERY_MAC     0x00A5
#define LD2450_ACK_BIT           0x0100

typedef struct {
    bool present;
    int16_t x_mm; // lateral offset from the sensor's boresight
    int16_t y_mm; // distance straight out from the sensor face
    int16_t speed_cm_s; // positive = moving away, negative = approaching
    uint16_t resolution_mm; // the sensor's distance gate size for this target
} Ld2450Target;

typedef struct {
    Ld2450Target target[LD2450_MAX_TARGETS];
} Ld2450Frame;

typedef struct {
    uint16_t command; // as echoed, LD2450_ACK_BIT included
    uint16_t status; // 0 = success
    const uint8_t* data; // response payload after the status word
    size_t data_len;
} Ld2450Ack;

typedef enum {
    Ld2450EventNone,
    Ld2450EventFrame, // parser->frame is valid
    Ld2450EventAck, // parser->ack is valid until the next feed
} Ld2450Event;

typedef struct {
    uint8_t buf[LD2450_MAX_ACK];
    size_t len;
    size_t expected; // total length once known, else 0
    Ld2450Frame frame;
    Ld2450Ack ack;
} Ld2450Parser;

void ld2450_parser_reset(Ld2450Parser* parser);

/** Feed one received byte. Resynchronises on its own after garbage. */
Ld2450Event ld2450_parser_feed(Ld2450Parser* parser, uint8_t byte);

/**
 * Build a command frame.
 * @return frame length, or 0 if out_size is too small
 */
size_t ld2450_build_command(
    uint8_t* out,
    size_t out_size,
    uint16_t command,
    const uint8_t* value,
    size_t value_len);

/** True if ack is a successful reply to command. */
bool ld2450_ack_ok(const Ld2450Ack* ack, uint16_t command);

/** Format a QUERY_VERSION reply as e.g. "V1.02.22062416". */
bool ld2450_ack_version(const Ld2450Ack* ack, char* out, size_t out_size);

/** Encode a data frame, the inverse of the parser. For demo mode and tests. */
void ld2450_encode_frame(uint8_t out[LD2450_FRAME_SIZE], const Ld2450Frame* frame);
