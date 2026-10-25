/**
 * @file ld2450.c
 * See ld2450.h.
 */
#include "ld2450.h"

#include <stdio.h>
#include <string.h>

static const uint8_t data_header[4] = {0xAA, 0xFF, 0x03, 0x00};
static const uint8_t data_footer[2] = {0x55, 0xCC};
static const uint8_t cmd_header[4] = {0xFD, 0xFC, 0xFB, 0xFA};
static const uint8_t cmd_footer[4] = {0x04, 0x03, 0x02, 0x01};

// Coordinates and speed are sign-magnitude with the sign bit inverted:
// bit 15 set means positive. An all-zero word marks an empty target slot.
static int16_t ld2450_decode_signed(uint8_t lo, uint8_t hi) {
    int16_t magnitude = (int16_t)(((hi & 0x7F) << 8) | lo);
    return (hi & 0x80) ? magnitude : (int16_t)-magnitude;
}

static void ld2450_encode_signed(uint8_t* out, int16_t value) {
    uint16_t raw = value >= 0 ? (uint16_t)(0x8000 | value) : (uint16_t)(-value);
    out[0] = raw & 0xFF;
    out[1] = raw >> 8;
}

void ld2450_parser_reset(Ld2450Parser* parser) {
    parser->len = 0;
    parser->expected = 0;
}

static void ld2450_decode_frame(Ld2450Parser* parser) {
    for(size_t i = 0; i < LD2450_MAX_TARGETS; i++) {
        const uint8_t* p = parser->buf + 4 + i * 8;
        Ld2450Target* t = &parser->frame.target[i];
        t->x_mm = ld2450_decode_signed(p[0], p[1]);
        t->y_mm = ld2450_decode_signed(p[2], p[3]);
        t->speed_cm_s = ld2450_decode_signed(p[4], p[5]);
        t->resolution_mm = (uint16_t)(p[6] | (p[7] << 8));
        t->present = t->x_mm != 0 || t->y_mm != 0;
        if(!t->present) {
            t->speed_cm_s = 0;
            t->resolution_mm = 0;
        }
    }
}

Ld2450Event ld2450_parser_feed(Ld2450Parser* parser, uint8_t byte) {
    // Match the 4-byte header first. A mismatch restarts the search, keeping
    // this byte if it could itself begin a header.
    if(parser->len < 4) {
        const uint8_t* header = NULL;
        if(parser->len == 0) {
            if(byte == data_header[0])
                header = data_header;
            else if(byte == cmd_header[0])
                header = cmd_header;
        } else {
            header = parser->buf[0] == data_header[0] ? data_header : cmd_header;
            if(byte != header[parser->len]) {
                parser->len = 0;
                return ld2450_parser_feed(parser, byte);
            }
        }
        if(!header) return Ld2450EventNone;
        parser->buf[parser->len++] = byte;
        if(parser->len == 4 && header == data_header) parser->expected = LD2450_FRAME_SIZE;
        return Ld2450EventNone;
    }

    parser->buf[parser->len++] = byte;
    bool is_data = parser->buf[0] == data_header[0];

    // A command frame's length is in bytes 4-5: payload, then a 4-byte footer.
    if(!is_data && parser->len == 6) {
        size_t payload = parser->buf[4] | (parser->buf[5] << 8);
        parser->expected = 6 + payload + 4;
        if(parser->expected > LD2450_MAX_ACK || payload < 4) {
            ld2450_parser_reset(parser);
            return Ld2450EventNone;
        }
    }
    if(!parser->expected || parser->len < parser->expected) return Ld2450EventNone;

    Ld2450Event event = Ld2450EventNone;
    if(is_data) {
        if(memcmp(parser->buf + LD2450_FRAME_SIZE - 2, data_footer, 2) == 0) {
            ld2450_decode_frame(parser);
            event = Ld2450EventFrame;
        }
    } else if(memcmp(parser->buf + parser->expected - 4, cmd_footer, 4) == 0) {
        parser->ack.command = (uint16_t)(parser->buf[6] | (parser->buf[7] << 8));
        parser->ack.status = (uint16_t)(parser->buf[8] | (parser->buf[9] << 8));
        parser->ack.data = parser->buf + 10;
        parser->ack.data_len = parser->expected - 4 - 10;
        event = Ld2450EventAck;
    }
    ld2450_parser_reset(parser);
    return event;
}

size_t ld2450_build_command(
    uint8_t* out,
    size_t out_size,
    uint16_t command,
    const uint8_t* value,
    size_t value_len) {
    size_t total = 4 + 2 + 2 + value_len + 4;
    if(out_size < total) return 0;
    size_t payload = 2 + value_len;
    memcpy(out, cmd_header, 4);
    out[4] = payload & 0xFF;
    out[5] = payload >> 8;
    out[6] = command & 0xFF;
    out[7] = command >> 8;
    if(value_len) memcpy(out + 8, value, value_len);
    memcpy(out + 8 + value_len, cmd_footer, 4);
    return total;
}

bool ld2450_ack_ok(const Ld2450Ack* ack, uint16_t command) {
    return ack->command == (command | LD2450_ACK_BIT) && ack->status == 0;
}

bool ld2450_ack_version(const Ld2450Ack* ack, char* out, size_t out_size) {
    // Payload: firmware type (2), major (2), minor (4), all little-endian.
    if(!ld2450_ack_ok(ack, LD2450_CMD_QUERY_VERSION) || ack->data_len < 8) return false;
    const uint8_t* d = ack->data;
    snprintf(out, out_size, "V%u.%02X.%02X%02X%02X%02X", d[3], d[2], d[7], d[6], d[5], d[4]);
    return true;
}

void ld2450_encode_frame(uint8_t out[LD2450_FRAME_SIZE], const Ld2450Frame* frame) {
    memcpy(out, data_header, 4);
    for(size_t i = 0; i < LD2450_MAX_TARGETS; i++) {
        uint8_t* p = out + 4 + i * 8;
        const Ld2450Target* t = &frame->target[i];
        if(!t->present) {
            memset(p, 0, 8);
            continue;
        }
        ld2450_encode_signed(p, t->x_mm);
        ld2450_encode_signed(p + 2, t->y_mm);
        ld2450_encode_signed(p + 4, t->speed_cm_s);
        p[6] = t->resolution_mm & 0xFF;
        p[7] = t->resolution_mm >> 8;
    }
    memcpy(out + LD2450_FRAME_SIZE - 2, data_footer, 2);
}
