#include "tele_radio_remote_core.h"

#include <stdio.h>
#include <string.h>

void trr_make_key_filename(char* buf, size_t size, uint32_t id24, uint8_t button) {
    snprintf(buf, size, "TRR_%06X_B%02X.sub", (unsigned int)(id24 & 0xFFFFFFu), (unsigned int)button);
}

/* Parse exactly `count` hex digits */
static bool read_hex_digits(const char* text, size_t count, uint32_t* value) {
    uint32_t result = 0;
    for(size_t i = 0; i < count; ++i) {
        char c = text[i];
        uint32_t digit;
        if(c >= '0' && c <= '9') digit = c - '0';
        else if(c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if(c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return false;
        result = (result << 4) | digit;
    }
    *value = result;
    return true;
}

bool trr_parse_key_filename(const char* name, uint32_t* id24, uint8_t* button) {
    uint32_t id, btn;
    if(!name || strlen(name) != 18 || strncmp(name, "TRR_", 4) != 0 ||
       strncmp(name + 10, "_B", 2) != 0 || strcmp(name + 14, ".sub") != 0 ||
       !read_hex_digits(name + 4, 6, &id) || !read_hex_digits(name + 12, 2, &btn))
        return false;
    if(id24) *id24 = id;
    if(button) *button = btn;
    return true;
}

uint16_t trr_crc16_kermit(const uint8_t* data, size_t len) {
    uint16_t crc = 0;
    for(size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for(int b = 0; b < 8; b++)
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0x8408) : (uint16_t)(crc >> 1);
    }
    return crc;
}

uint64_t trr_make_key(uint32_t id24, uint8_t button) {
    return ((uint64_t)(id24 & 0xFFFFFFu) << 8) | button;
}

uint32_t trr_key_id(uint64_t key) {
    return (uint32_t)(key >> 8) & 0xFFFFFFu;
}

uint8_t trr_key_button(uint64_t key) {
    return (uint8_t)(key & 0xFFu);
}

/* 11 frame bytes for a key FF FF | ID | 00 00 | BTN | 00 | CRC_HI CRC_LO */
static void trr_build_frame_bytes(uint64_t key, uint8_t out[TRR_FRAME_BYTES]) {
    uint8_t body[7] = {
        (uint8_t)(trr_key_id(key) >> 16),
        (uint8_t)(trr_key_id(key) >> 8),
        (uint8_t)(trr_key_id(key)),
        0x00,
        0x00,
        trr_key_button(key),
        0x00,
    };
    uint16_t crc = trr_crc16_kermit(body, sizeof(body));
    out[0] = 0xFF;
    out[1] = 0xFF;
    memcpy(&out[2], body, sizeof(body));
    out[9] = (uint8_t)(crc >> 8); /* big-endian */
    out[10] = (uint8_t)(crc & 0xFF);
}

size_t trr_build_frame_cells(uint64_t key, uint8_t* out_cells) {
    uint8_t bytes[TRR_FRAME_BYTES];
    trr_build_frame_bytes(key, bytes);
    size_t n = 0;
    for(size_t i = 0; i < 12; i++) /* idle HIGH */
        out_cells[n++] = 1;
    for(size_t b = 0; b < TRR_FRAME_BYTES; b++) {
        out_cells[n++] = 0; /* start */
        for(int i = 0; i < 8; i++) /* data, LSB first */
            out_cells[n++] = (uint8_t)((bytes[b] >> i) & 1);
        out_cells[n++] = 1; /* stop */
    }
    return n; /* == TRR_FRAME_CELLS */
}

bool trr_parse_frame_cells(const uint8_t* cells, uint64_t* key_out) {
    for(size_t i = 0; i < 12; i++)
        if(cells[i] != 1)
            return false;
    uint8_t bytes[TRR_FRAME_BYTES];
    for(size_t b = 0; b < TRR_FRAME_BYTES; b++) {
        size_t base = 12 + 10 * b;
        if(cells[base] != 0 || cells[base + 9] != 1) /* start/stop */
            return false;
        uint8_t v = 0;
        for(int i = 0; i < 8; i++)
            v |= (uint8_t)(cells[base + 1 + i] << i); /* LSB first */
        bytes[b] = v;
    }
    if(bytes[0] != 0xFF || bytes[1] != 0xFF)
        return false;
    uint16_t crc = trr_crc16_kermit(&bytes[2], 7);
    if(((uint16_t)((bytes[9] << 8) | bytes[10])) != crc)
        return false;
    /* 0,1=FF; 2,3,4=ID; 5,6=00; 7=BTN; 8=00; 9,10=CRC */
    if(bytes[5] != 0 || bytes[6] != 0 || bytes[8] != 0)
        return false;
    uint32_t id = ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 8) | bytes[4];
    if(key_out)
        *key_out = trr_make_key(id, bytes[7]);
    return true;
}

void trr_decoder_init(TRRDecoder* d) {
    memset(d, 0, sizeof(*d));
}

void trr_decoder_feed_cell(TRRDecoder* d, uint8_t level) {
    if(d->len == sizeof(d->buf)) { /* shift out old cells */
        memmove(d->buf, d->buf + 128, sizeof(d->buf) - 128);
        d->len -= 128;
        if(d->pending && d->frame_start >= 128)
            d->frame_start -= 128;
        else if(d->pending)
            d->pending = false;
    }
    size_t prev_run1 = d->run1;
    d->buf[d->len++] = level ? 1 : 0;
    d->run1 = level ? prev_run1 + 1 : 0;

    if(!level && prev_run1 >= 12) {
        d->frame_start = d->len - 13;
        d->pending = true;
    }
    if(d->pending && d->len >= d->frame_start + TRR_FRAME_CELLS) {
        uint64_t key;
        if(trr_parse_frame_cells(&d->buf[d->frame_start], &key)) {
            d->key = key;
            d->ready = true;
        }
        d->pending = false;
    }
}

bool trr_decoder_poll(TRRDecoder* d, uint64_t* key_out) {
    if(!d->ready)
        return false;
    d->ready = false;
    if(key_out)
        *key_out = d->key;
    return true;
}
