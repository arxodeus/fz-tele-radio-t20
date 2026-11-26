/* Host model of the async-TX HAL. The yield callback is drained by furi_delay_ms(). */
#pragma once
#include <toolbox/level_duration.h>
typedef LevelDuration (*FuriHalSubGhzAsyncTxCallback)(void* context);
typedef void (*FuriHalSubGhzCaptureCallback)(bool level, uint32_t duration, void* context);
void furi_hal_subghz_reset(void);
void furi_hal_subghz_idle(void);
void furi_hal_subghz_sleep(void);
void furi_hal_subghz_rx(void);
void furi_hal_subghz_flush_rx(void);
void furi_hal_subghz_load_custom_preset(const uint8_t* preset_data);
uint32_t furi_hal_subghz_set_frequency_and_path(uint32_t value);
void furi_hal_subghz_start_async_rx(FuriHalSubGhzCaptureCallback callback, void* context);
void furi_hal_subghz_stop_async_rx(void);
bool furi_hal_subghz_start_async_tx(FuriHalSubGhzAsyncTxCallback callback, void* context);
bool furi_hal_subghz_is_async_tx_complete(void);
void furi_hal_subghz_stop_async_tx(void);
