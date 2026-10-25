/**
 * @file marauder_proto.c
 * See marauder_proto.h for the line formats.
 */
#include "marauder_proto.h"

#include <stdlib.h>
#include <string.h>

#define MP_MAC_STR_LEN 17

static int mp_hex_digit(char c) {
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool mp_hex_byte(const char* s, uint8_t* out) {
    int hi = mp_hex_digit(s[0]), lo = mp_hex_digit(s[1]);
    if(hi < 0 || lo < 0) return false;
    *out = (uint8_t)(hi << 4 | lo);
    return true;
}

bool mp_parse_mac(const char* s, uint8_t out[6]) {
    for(int i = 0; i < 6; i++) {
        if(!mp_hex_byte(s + i * 3, &out[i])) return false;
        if(i < 5 && s[i * 3 + 2] != ':') return false;
    }
    return true;
}

bool mp_mac_is_group(const uint8_t mac[6]) {
    return mac[0] & 0x01;
}

static bool mp_starts_with(const char* s, const char* prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

// "-78 Ch: 3 <bssid> ESSID: <ssid> <cap_lo> <cap_hi>"
static bool mp_parse_ap(const char* p, MpAp* ap) {
    char* end;
    long rssi = strtol(p, &end, 10);
    if(end == p || rssi < -128 || rssi > 0 || !mp_starts_with(end, " Ch: ")) return false;
    p = end + 5;
    long channel = strtol(p, &end, 10);
    if(end == p || channel < 1 || channel > 196 || *end != ' ') return false;
    p = end + 1;
    if(!mp_parse_mac(p, ap->bssid)) return false;
    p += MP_MAC_STR_LEN;
    if(!mp_starts_with(p, " ESSID: ")) return false;
    p += 8;

    // Drop trailing whitespace; the capability bytes are then the last
    // " xx xx". Everything before them is the SSID field.
    size_t len = strlen(p);
    while(len && (p[len - 1] == ' ' || p[len - 1] == '\r'))
        len--;
    uint8_t cap_lo, cap_hi;
    if(len < 6 || p[len - 6] != ' ' || p[len - 3] != ' ' || !mp_hex_byte(p + len - 5, &cap_lo) ||
       !mp_hex_byte(p + len - 2, &cap_hi)) {
        return false;
    }
    size_t field_len = len - 6;

    // A hidden network prints an empty SSID followed by its own BSSID.
    char bssid_str[MP_MAC_STR_LEN + 1];
    memcpy(bssid_str, p - 8 - MP_MAC_STR_LEN, MP_MAC_STR_LEN);
    bssid_str[MP_MAC_STR_LEN] = 0;
    ap->hidden = field_len == MP_MAC_STR_LEN + 1 && p[0] == ' ' &&
                 strncmp(p + 1, bssid_str, MP_MAC_STR_LEN) == 0;
    if(ap->hidden || field_len == 0) {
        ap->hidden = true;
        ap->ssid[0] = 0;
    } else {
        if(field_len > MP_SSID_MAX) field_len = MP_SSID_MAX;
        memcpy(ap->ssid, p, field_len);
        ap->ssid[field_len] = 0;
    }

    ap->rssi = (int8_t)rssi;
    ap->channel = (uint8_t)channel;
    ap->encrypted = cap_lo & 0x10; // capability Privacy bit
    (void)cap_hi;
    return true;
}

// "1: sta: <mac> -> ap: <mac>" or "2: ap: <mac> -> sta: <mac>"
static bool mp_parse_station(const char* p, MpStation* st) {
    char* end;
    strtol(p, &end, 10);
    if(end == p || !mp_starts_with(end, ": ")) return false;
    p = end + 2;

    bool sta_first;
    if(mp_starts_with(p, "sta: ")) {
        sta_first = true;
    } else if(mp_starts_with(p, "ap: ")) {
        sta_first = false;
    } else {
        return false;
    }
    p += sta_first ? 5 : 4;
    uint8_t first[6], second[6];
    if(!mp_parse_mac(p, first)) return false;
    p += MP_MAC_STR_LEN;
    const char* arrow = sta_first ? " -> ap: " : " -> sta: ";
    if(!mp_starts_with(p, arrow)) return false;
    p += strlen(arrow);
    if(!mp_parse_mac(p, second)) return false;

    memcpy(st->station, sta_first ? first : second, 6);
    memcpy(st->ap, sta_first ? second : first, 6);
    return true;
}

static void mp_copy_text(char* out, size_t size, const char* src) {
    size_t n = strlen(src);
    while(n && (src[n - 1] == ' ' || src[n - 1] == '\r'))
        n--;
    if(n >= size) n = size - 1;
    memcpy(out, src, n);
    out[n] = 0;
}

bool mp_parse_line(const char* line, MpLine* out) {
    memset(out, 0, sizeof(*out));
    while(*line == '>' || *line == ' ')
        line++;

    if(mp_starts_with(line, "Version: ")) {
        out->type = MpLineVersion;
        mp_copy_text(out->text, sizeof(out->text), line + 9);
        return true;
    }
    if(mp_starts_with(line, "Firmware: ")) {
        out->type = MpLineFirmware;
        mp_copy_text(out->text, sizeof(out->text), line + 10);
        return true;
    }
    if(mp_parse_ap(line, &out->ap)) {
        out->type = MpLineAp;
        return true;
    }
    if(mp_parse_station(line, &out->station)) {
        out->type = MpLineStation;
        return true;
    }
    return false;
}
