#pragma once

/* Flipper HAL for the radio with custom CC1101 preset, async RX and async TX. */

#include <furi.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*TRRRxEdgeCallback)(bool level, uint32_t duration_us, void* ctx);

/* CC1101 custom preset register blob. */
#define TRR_CC1101_PRESET_LEN 46u
extern const uint8_t trr_cc1101_preset[TRR_CC1101_PRESET_LEN];

/* Reset radio, load the custom preset and set `freq_hz`. */
void tele_radio_remote_radio_init(uint32_t freq_hz);

/* Start edge capture at `freq_hz`. Callback runs in ISR context. */
void tele_radio_remote_radio_rx_start(TRRRxEdgeCallback cb, void* ctx, uint32_t freq_hz);
/* Stop edge capture. Safe to call when RX is not running. */
void tele_radio_remote_radio_rx_stop(void);

typedef enum { TRRTxComplete, TRRTxBlocked, TRRTxTimeout, TRRTxCancelled } TRRTxResult;

/* Called with zero before TX starts, then from the radio ISR after each
 * queued frame. Return true to queue the next frame. */
typedef bool (*TRRTxContinueCallback)(uint32_t frames_queued, void* ctx);
/* Called from the calling thread while transmitting, when the count changes. */
typedef void (*TRRTxProgressCallback)(uint32_t frames_queued, void* ctx);

/* `should_continue` decides whether to start and whether to keep going at
 * each frame boundary. At most `max_frames` are sent. */
TRRTxResult tele_radio_remote_radio_tx_stream(
    uint64_t key,
    uint32_t freq_hz,
    uint32_t max_frames,
    TRRTxContinueCallback should_continue,
    TRRTxProgressCallback progress,
    void* ctx,
    uint32_t* frames_out);

void tele_radio_remote_radio_deinit(void);

#ifdef __cplusplus
}
#endif
