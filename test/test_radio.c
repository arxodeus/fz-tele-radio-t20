#include "../tele_radio_remote_core.h"
#include "../tele_radio_remote_radio.h"

#include <furi_hal_subghz.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond)                                                \
    do {                                                           \
        if(!(cond)) {                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                            \
        }                                                          \
    } while(0)

/* ---- HAL model ---- */
static FuriHalSubGhzAsyncTxCallback hal_cb;
static void* hal_ctx;
static bool hal_tx_allowed = true, hal_in_tx, hal_done, hal_stall;
static uint32_t hal_tx_starts;
static uint32_t hal_tick;
#define MAX_RUNS 200000
static LevelDuration out[MAX_RUNS];
static size_t out_n;

void furi_delay_ms(uint32_t ms) {
    hal_tick += ms;
    /* the DMA consumes ~10 ms of signal per 10 ms of wall time */
    uint32_t budget = ms * 1000;
    while(hal_in_tx && !hal_done && !hal_stall && budget > 0) {
        LevelDuration ld = hal_cb(hal_ctx);
        if(level_duration_is_reset(ld)) {
            hal_done = true;
            break;
        }
        if(out_n < MAX_RUNS) out[out_n++] = ld;
        uint32_t d = level_duration_get_duration(ld);
        budget = d >= budget ? 0 : budget - d;
    }
}
uint32_t furi_get_tick(void) {
    return hal_tick;
}
void furi_hal_subghz_reset(void) {}
void furi_hal_subghz_idle(void) {}
void furi_hal_subghz_sleep(void) {}
void furi_hal_subghz_rx(void) {}
void furi_hal_subghz_flush_rx(void) {}
void furi_hal_subghz_load_custom_preset(const uint8_t* p) {
    (void)p;
}
uint32_t furi_hal_subghz_set_frequency_and_path(uint32_t v) {
    return v;
}
void furi_hal_subghz_start_async_rx(FuriHalSubGhzCaptureCallback c, void* x) {
    (void)c;
    (void)x;
}
void furi_hal_subghz_stop_async_rx(void) {}
bool furi_hal_subghz_start_async_tx(FuriHalSubGhzAsyncTxCallback cb, void* ctx) {
    CHECK(!hal_in_tx);
    hal_tx_starts++;
    if(!hal_tx_allowed) return false;
    hal_cb = cb;
    hal_ctx = ctx;
    hal_in_tx = true;
    hal_done = false;
    out_n = 0;
    return true;
}
bool furi_hal_subghz_is_async_tx_complete(void) {
    return hal_done;
}
void furi_hal_subghz_stop_async_tx(void) {
    CHECK(hal_in_tx);
    hal_in_tx = false;
}

/* ---- press models ---- */
typedef struct {
    uint32_t stop_after; /* continue while frames < stop_after */
    uint32_t calls;
    uint32_t progress_calls;
    bool cancel_before_tx;
} Press;

static bool cont(uint32_t frames, void* ctx) {
    Press* p = ctx;
    if(frames == 0) return !p->cancel_before_tx;
    p->calls++;
    return frames < p->stop_after;
}
static void progress(uint32_t frames, void* ctx) {
    (void)frames;
    ((Press*)ctx)->progress_calls++;
}

/* Expand the captured runs into cells and compare to N back-to-back frames. */
static bool output_is_frames(uint64_t key, uint32_t frames) {
    static uint8_t cells[MAX_RUNS];
    size_t n = 0;
    for(size_t i = 0; i < out_n; i++) {
        if(i > 0 && level_duration_get_level(out[i]) == level_duration_get_level(out[i - 1]))
            return false; /* two runs of the same level */
        uint32_t d = level_duration_get_duration(out[i]);
        if(d % TRR_TE_US) return false;
        for(uint32_t c = 0; c < d / TRR_TE_US; c++)
            cells[n++] = level_duration_get_level(out[i]);
    }
    if(n != (size_t)frames * TRR_FRAME_CELLS) return false;
    uint8_t frame[TRR_FRAME_CELLS];
    trr_build_frame_cells(key, frame);
    for(uint32_t f = 0; f < frames; f++)
        if(memcmp(&cells[f * TRR_FRAME_CELLS], frame, TRR_FRAME_CELLS) != 0) return false;
    return true;
}

int main(void) {
    uint64_t key = trr_make_key(0x5A3C91, 0x01);
    uint32_t sent = 0;

    /* tap with Repeats = 1 .. 100 exactly that many gap-free frames */
    uint32_t taps[] = {1, 2, 10, 100};
    for(size_t t = 0; t < 4; t++) {
        Press p = {.stop_after = taps[t]};
        TRRTxResult r = tele_radio_remote_radio_tx_stream(
            key, TRR_FREQUENCY_HZ, TRR_TX_MAX_FRAMES, cont, progress, &p, &sent);
        CHECK(r == TRRTxComplete);
        CHECK(sent == taps[t]);
        CHECK(output_is_frames(key, taps[t]));
        CHECK(!hal_in_tx);
    }

    /* a stuck hold stops at the safety limit */
    {
        Press p = {.stop_after = UINT32_MAX};
        TRRTxResult r = tele_radio_remote_radio_tx_stream(
            key, TRR_FREQUENCY_433_HZ, TRR_TX_MAX_FRAMES, cont, progress, &p, &sent);
        CHECK(r == TRRTxComplete);
        CHECK(sent == TRR_TX_MAX_FRAMES);
        CHECK(output_is_frames(key, TRR_TX_MAX_FRAMES));
        CHECK(p.progress_calls > 0);
        CHECK((uint64_t)sent * TRR_FRAME_US <= TRR_TX_MAX_SECONDS * 1000000ull);
    }

    /* cancel before the first frame never starts TX */
    {
        Press p = {.cancel_before_tx = true};
        uint32_t starts = hal_tx_starts;
        TRRTxResult r =
            tele_radio_remote_radio_tx_stream(key, TRR_FREQUENCY_HZ, 5, cont, NULL, &p, &sent);
        CHECK(r == TRRTxCancelled && sent == 0 && hal_tx_starts == starts);
    }

    /* cancel at the first boundary still sends one whole frame */
    {
        Press p = {.stop_after = 0};
        tele_radio_remote_radio_tx_stream(key, TRR_FREQUENCY_HZ, 5, cont, NULL, &p, &sent);
        CHECK(sent == 1 && output_is_frames(key, 1));
    }

    /* region block, no TX state entered, stop_async_tx never called */
    {
        hal_tx_allowed = false;
        Press p = {.stop_after = 10};
        TRRTxResult r =
            tele_radio_remote_radio_tx_stream(key, TRR_FREQUENCY_HZ, 10, cont, NULL, &p, &sent);
        CHECK(r == TRRTxBlocked && sent == 0 && p.calls == 0 && !hal_in_tx);
        hal_tx_allowed = true;
    }

    /* stalled DMA, watchdog stops the radio instead of hanging */
    {
        hal_stall = true;
        Press p = {.stop_after = UINT32_MAX};
        uint32_t start = hal_tick;
        TRRTxResult r = tele_radio_remote_radio_tx_stream(
            key, TRR_FREQUENCY_HZ, TRR_TX_MAX_FRAMES, cont, NULL, &p, &sent);
        CHECK(r == TRRTxTimeout && !hal_in_tx);
        CHECK(hal_tick - start < 2000);
        hal_stall = false;
    }

    if(failures == 0) printf("ALL RADIO TESTS PASSED\n");
    return failures ? 1 : 0;
}
