#include "../tele_radio_remote_core.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond)                                              \
    do {                                                         \
        if(!(cond)) {                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                          \
        }                                                        \
    } while(0)

static size_t bits_from_str(const char* s, uint8_t* out) {
    size_t n = strlen(s);
    for(size_t i = 0; i < n; i++)
        out[i] = (uint8_t)(s[i] == '1');
    return n;
}

/* 122-cell frames for ID 0x5A3C91 */
static const char* CAP_BTN1 =
    "11111111111101111111110111111111001011010100011110010100010011000000000100000000010100000001000000000101000001010101011001";
static const char* CAP_BTN2 =
    "11111111111101111111110111111111001011010100011110010100010011000000000100000000010010000001000000000101101011010101110101";
static const char* CAP_BTN3 =
    "11111111111101111111110111111111001011010100011110010100010011000000000100000000010001000001000000000101111110010101100011";

int main(void) {
    /* CRC check value */
    CHECK(trr_crc16_kermit((const uint8_t*)"123456789", 9) == 0x2189);

    /* key packing */
    uint64_t k1 = trr_make_key(0x5A3C91, 0x01);
    CHECK(trr_key_id(k1) == 0x5A3C91 && trr_key_button(k1) == 0x01);

    /* encoder reproduces the reference frames bit-exactly */
    uint8_t cells[TRR_FRAME_CELLS];
    uint8_t cap[TRR_FRAME_CELLS];
    const char* caps[3] = {CAP_BTN1, CAP_BTN2, CAP_BTN3};
    uint8_t btns[3] = {0x01, 0x02, 0x04};
    for(int t = 0; t < 3; t++) {
        size_t n = trr_build_frame_cells(trr_make_key(0x5A3C91, btns[t]), cells);
        CHECK(n == TRR_FRAME_CELLS);
        size_t m = bits_from_str(caps[t], cap);
        CHECK(m == TRR_FRAME_CELLS);
        CHECK(memcmp(cells, cap, TRR_FRAME_CELLS) == 0);
        /* parser accepts the same frame */
        uint64_t key = 0;
        CHECK(trr_parse_frame_cells(cap, &key));
        CHECK(key == trr_make_key(0x5A3C91, btns[t]));
    }

    /* parser rejects a corrupted frame */
    uint8_t bad[TRR_FRAME_CELLS];
    bits_from_str(CAP_BTN1, bad);
    bad[40] ^= 1;
    CHECK(!trr_parse_frame_cells(bad, NULL));

    /* feed frame prefixed with one extra HIGH and trailed by the next frame's idle run */
    TRRDecoder d;
    uint8_t c_stream[TRR_FRAME_CELLS];
    trr_decoder_init(&d);
    uint8_t stream[TRR_FRAME_CELLS * 2 + 16];
    size_t sn = 0;
    stream[sn++] = 1;
    bits_from_str(CAP_BTN1, stream + sn);
    sn += TRR_FRAME_CELLS;
    memset(stream + sn, 1, 14);
    sn += 14;
    for(size_t i = 0; i < sn; i++)
        trr_decoder_feed_cell(&d, stream[i]);
    uint64_t key = 0;
    CHECK(trr_decoder_poll(&d, &key));
    CHECK(key == trr_make_key(0x5A3C91, 0x01));
    CHECK(!trr_decoder_poll(&d, &key));

    /* first frame of a burst after silence */
    trr_decoder_init(&d);
    for(int i = 0; i < 20; i++)
        trr_decoder_feed_cell(&d, 0);
    bits_from_str(CAP_BTN2, cap);
    for(size_t i = 0; i < TRR_FRAME_CELLS; i++)
        trr_decoder_feed_cell(&d, cap[i]);
    trr_decoder_feed_cell(&d, 1);
    CHECK(trr_decoder_poll(&d, &key));
    CHECK(key == trr_make_key(0x5A3C91, 0x02));

    /* decoder survives many back-to-back frames across buffer shifts */
    trr_decoder_init(&d);
    int decoded = 0;
    for(int f = 0; f < 20; f++) {
        trr_build_frame_cells(trr_make_key(0x112233, 0x04), c_stream);
        for(size_t i = 0; i < TRR_FRAME_CELLS; i++)
            trr_decoder_feed_cell(&d, c_stream[i]);
        if(trr_decoder_poll(&d, &key) && key == trr_make_key(0x112233, 0x04)) decoded++;
    }
    CHECK(decoded == 20);

    /* synthetic ID 11 22 33, buttons 1/2/3 -> CRC 77EF/5D87/0957 */
    uint8_t c[TRR_FRAME_CELLS];
    uint16_t expect_crc[3] = {0x77EF, 0x5D87, 0x0957};
    for(int t = 0; t < 3; t++) {
        trr_build_frame_cells(trr_make_key(0x112233, btns[t]), c);
        uint64_t key = 0;
        CHECK(trr_parse_frame_cells(c, &key));
        CHECK(key == trr_make_key(0x112233, btns[t]));
        /* CRC bytes on the wire are big-endian
         * byte 9 (CRC_HI) data at cells 103..110, byte 10 (CRC_LO) at 113..120 */
        uint8_t crc_hi = 0, crc_lo = 0;
        for(int i = 0; i < 8; i++) {
            crc_hi |= (uint8_t)(c[103 + i] << i);
            crc_lo |= (uint8_t)(c[113 + i] << i);
        }
        CHECK(((uint16_t)((crc_hi << 8) | crc_lo)) == expect_crc[t]);
    }

    /* key filename format round-trip */
    {
        char name[64];
        uint32_t id = 0;
        uint8_t btn = 0;
        trr_make_key_filename(name, sizeof(name), 0x5A3C91, 0x04);
        CHECK(strcmp(name, "TRR_5A3C91_B04.sub") == 0);
        CHECK(trr_parse_key_filename(name, &id, &btn));
        CHECK(id == 0x5A3C91 && btn == 0x04);
        CHECK(!trr_parse_key_filename("TRR_5A3C91_B04.txt", &id, &btn));
        CHECK(!trr_parse_key_filename("TRR_1_B04.sub", &id, &btn));
        CHECK(!trr_parse_key_filename("TRR_000001_B4.sub", &id, &btn));
        CHECK(!trr_parse_key_filename("TRR_-00001_B04.sub", &id, &btn));
        CHECK(!trr_parse_key_filename("TRR_ 00001_B04.sub", &id, &btn));
        CHECK(!trr_parse_key_filename("TRR_0x0001_B04.sub", &id, &btn));
        CHECK(trr_parse_key_filename("TRR_abcdef_B0a.sub", &id, &btn));
        CHECK(id == 0xabcdef && btn == 0x0a);
        CHECK(!trr_parse_key_filename("foo.sub", &id, &btn));
        CHECK(!trr_parse_key_filename(NULL, &id, &btn));
    }

    if(failures == 0)
        printf("ALL TESTS PASSED\n");
    return failures ? 1 : 0;
}
