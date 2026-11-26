#include "tele_radio_remote_radio.h"

#include <furi_hal_subghz.h>
#include <string.h>
#include <toolbox/level_duration.h>

#include "tele_radio_remote_core.h"

/* CC1101 custom preset 2-FSK, RX BW 270.8 kHz, deviation around 15.9 kHz,
 * GDO0 = async serial data, async continuous mode. Register pairs, 00 00
 * terminator, then the 8-byte PATABLE. */
const uint8_t trr_cc1101_preset[TRR_CC1101_PRESET_LEN] = {
    0x02, 0x0D, 0x0B, 0x06, 0x08, 0x32, 0x07, 0x04, 0x14, 0x00, 0x13, 0x02,
    0x12, 0x04, 0x11, 0x36, 0x10, 0x69, 0x15, 0x32, 0x18, 0x18, 0x19, 0x16,
    0x1D, 0x91, 0x1C, 0x00, 0x1B, 0x07, 0x20, 0xFB, 0x22, 0x10, 0x21, 0x56,
    0x00, 0x00, /* terminator */
    0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* PATABLE */
};
_Static_assert(sizeof(trr_cc1101_preset) == TRR_CC1101_PRESET_LEN, "preset length mismatch");

static void tele_radio_remote_radio_load(uint32_t freq_hz) {
    furi_hal_subghz_load_custom_preset(trr_cc1101_preset);
    furi_hal_subghz_set_frequency_and_path(freq_hz);
}

void tele_radio_remote_radio_init(uint32_t freq_hz) {
    furi_hal_subghz_reset();
    tele_radio_remote_radio_load(freq_hz);
}

static bool rx_active = false;

void tele_radio_remote_radio_rx_start(TRRRxEdgeCallback cb, void* ctx, uint32_t freq_hz) {
    tele_radio_remote_radio_load(freq_hz);
    furi_hal_subghz_flush_rx();
    furi_hal_subghz_rx();
    furi_hal_subghz_start_async_rx(cb, ctx);
    rx_active = true;
}

void tele_radio_remote_radio_rx_stop(void) {
    /* Only stop when we actually started it. */
    if(rx_active) {
        furi_hal_subghz_stop_async_rx();
        rx_active = false;
    }
    furi_hal_subghz_idle();
}

/* If progress stops for this long, the DMA is stuck. */
#define TRR_TX_STALL_MS 1000u

/* One frame as merged level runs. Shared with the ISR. */
static LevelDuration tx_upload[TRR_FRAME_CELLS];
static size_t tx_size = 0;
static size_t tx_front = 0;
static LevelDuration tx_joined; /* stop-bit run + next frame's idle run */
static uint32_t tx_max_frames = 0;
static volatile uint32_t tx_frames = 0;
static volatile bool tx_done = false;
static TRRTxContinueCallback tx_continue = NULL;
static void* tx_ctx = NULL;

/* ISR context. */
static LevelDuration tx_yield(void* ctx) {
    UNUSED(ctx);
    if(tx_done) return level_duration_reset();
    if(tx_front + 1 < tx_size) return tx_upload[tx_front++];
    /* Last run of the frame, the frame is now fully queued. */
    uint32_t frames = tx_frames + 1;
    tx_frames = frames;
    if(frames < tx_max_frames && tx_continue(frames, tx_ctx)) {
        tx_front = 1; /* run 0 is inside tx_joined */
        return tx_joined;
    }
    tx_done = true;
    return tx_upload[tx_size - 1];
}

TRRTxResult tele_radio_remote_radio_tx_stream(
    uint64_t key,
    uint32_t freq_hz,
    uint32_t max_frames,
    TRRTxContinueCallback should_continue,
    TRRTxProgressCallback progress,
    void* ctx,
    uint32_t* frames_out) {
    furi_check(should_continue && max_frames >= 1);
    uint8_t cells[TRR_FRAME_CELLS];
    trr_build_frame_cells(key, cells);

    /* merge runs of equal level into one LevelDuration each */
    size_t n = 0;
    size_t i = 0;
    while(i < TRR_FRAME_CELLS) {
        size_t j = i;
        while(j < TRR_FRAME_CELLS && cells[j] == cells[i])
            j++;
        tx_upload[n++] = level_duration_make(cells[i] != 0, (uint32_t)(j - i) * TRR_TE_US);
        i = j;
    }
    /* A frame starts with idle HIGH and ends with a stop bit, so
     * consecutive frames join into one HIGH run with no gap. */
    furi_check(n >= 3 && cells[0] == 1 && cells[TRR_FRAME_CELLS - 1] == 1);
    tx_joined = level_duration_make(
        true,
        level_duration_get_duration(tx_upload[n - 1]) + level_duration_get_duration(tx_upload[0]));
    tx_size = n;
    tx_front = 0;
    tx_frames = 0;
    tx_done = false;
    tx_max_frames = max_frames;
    tx_continue = should_continue;
    tx_ctx = ctx;

    tele_radio_remote_radio_load(freq_hz);
    if(!should_continue(0, ctx)) {
        if(frames_out) *frames_out = 0;
        return TRRTxCancelled;
    }
    /* Fails when the region blocks TX. */
    if(!furi_hal_subghz_start_async_tx(tx_yield, NULL)) {
        furi_hal_subghz_idle();
        if(frames_out) *frames_out = 0;
        return TRRTxBlocked;
    }
    TRRTxResult result = TRRTxComplete;
    uint32_t seen = 0;
    uint32_t last_progress = furi_get_tick();
    while(!furi_hal_subghz_is_async_tx_complete()) {
        furi_delay_ms(10);
        uint32_t frames = tx_frames;
        if(frames != seen) {
            seen = frames;
            last_progress = furi_get_tick();
            if(progress) progress(frames, ctx);
        } else if(furi_get_tick() - last_progress > furi_ms_to_ticks(TRR_TX_STALL_MS)) {
            result = TRRTxTimeout;
            break;
        }
    }
    furi_hal_subghz_stop_async_tx();
    furi_hal_subghz_idle();
    if(frames_out) *frames_out = tx_frames;
    return result;
}

void tele_radio_remote_radio_deinit(void) {
    furi_hal_subghz_idle();
    furi_hal_subghz_sleep();
}
