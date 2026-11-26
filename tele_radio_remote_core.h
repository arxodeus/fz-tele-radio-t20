#pragma once

/*
 * Logic core for the Tele Radio T20 protocol.
 *
 * Wire format:
 *   - 869.85 MHz or 433.92 MHz 2-FSK, bit cell 423 us
 *   - frame = 12 idle HIGH cells + 11 bytes, each byte framed 8N1
 *     (start 0, 8 data bits LSB-first, stop 1)  => 122 cells ~ 51.6 ms
 *   - bytes = FF FF | ID0 ID1 ID2 | 00 00 | BTN | 00 | CRC_HI CRC_LO
 *   - CRC-16/KERMIT over ID0 ID1 ID2 00 00 BTN 00, sent big-endian
 *   - BTN one-hot = 0x01 / 0x02 / 0x04
 *
 * Key packing:
 *   bits 31..8 = 24-bit remote ID (transmission order ID0 ID1 ID2)
 *   bits  7..0 = button byte
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TRR_FREQUENCY_HZ     869850000u /* 869 MHz (default) */
#define TRR_FREQUENCY_433_HZ 433920000u /* 433 MHz */
#define TRR_TE_US        423u
#define TRR_FRAME_CELLS  122u
#define TRR_KEY_BITS     32u
#define TRR_FRAME_BYTES  11u
#define TRR_TX_REPEATS   1u /* default */
#define TRR_FRAME_US     (TRR_FRAME_CELLS * TRR_TE_US) /* 51606 us */
/* One press never transmits more than 581 frames. */
#define TRR_TX_MAX_SECONDS 30u
#define TRR_TX_MAX_FRAMES  ((TRR_TX_MAX_SECONDS * 1000000u) / TRR_FRAME_US)

#ifdef __cplusplus
extern "C" {
#endif

/* CRC-16/KERMIT */
uint16_t trr_crc16_kermit(const uint8_t* data, size_t len);

/* id | button */
uint64_t trr_make_key(uint32_t id24, uint8_t button);
uint32_t trr_key_id(uint64_t key);
uint8_t trr_key_button(uint64_t key);

/* Build one frame as 122 cells.
 * out_cells must hold >= TRR_FRAME_CELLS bytes. Returns TRR_FRAME_CELLS. */
size_t trr_build_frame_cells(uint64_t key, uint8_t* out_cells);

/* Parse 122 cells. Returns true and the packed key if sync + 8N1 + CRC valid. */
bool trr_parse_frame_cells(const uint8_t* cells, uint64_t* key_out);

/* Saved-key filename format TRR_<ID6hex>_B<btn2hex>.sub */
void trr_make_key_filename(char* buf, size_t size, uint32_t id24, uint8_t button);
bool trr_parse_key_filename(const char* name, uint32_t* id24, uint8_t* button);

/* Streaming decoder over quantized cells (one call per 423 us cell). */
typedef struct {
    uint8_t buf[256];   /* recent cells */
    size_t len;         /* cells in buf */
    size_t run1;        /* consecutive HIGH cells at tail of buf */
    size_t frame_start; /* cell index in buf where a candidate frame starts */
    bool pending;
    bool ready;
    uint64_t key;
} TRRDecoder;

void trr_decoder_init(TRRDecoder* d);
void trr_decoder_feed_cell(TRRDecoder* d, uint8_t level); /* 1=HIGH, 0=LOW */
bool trr_decoder_poll(TRRDecoder* d, uint64_t* key_out);   /* true once per valid frame */

#ifdef __cplusplus
}
#endif
