#pragma once

#include "base.h"

#define SUBGHZ_PROTOCOL_TELE_RADIO_REMOTE_NAME "TeleRadioRemote"

typedef struct SubGhzProtocolDecoderTeleRadioRemote SubGhzProtocolDecoderTeleRadioRemote;
typedef struct SubGhzProtocolEncoderTeleRadioRemote SubGhzProtocolEncoderTeleRadioRemote;

extern const SubGhzProtocolDecoder subghz_protocol_tele_radio_remote_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_tele_radio_remote_encoder;
extern const SubGhzProtocol subghz_protocol_tele_radio_remote;

void* subghz_protocol_encoder_tele_radio_remote_alloc(SubGhzEnvironment* environment);
void subghz_protocol_encoder_tele_radio_remote_free(void* context);
SubGhzProtocolStatus subghz_protocol_encoder_tele_radio_remote_deserialize(
    void* context,
    FlipperFormat* flipper_format);
void subghz_protocol_encoder_tele_radio_remote_stop(void* context);
LevelDuration subghz_protocol_encoder_tele_radio_remote_yield(void* context);

void* subghz_protocol_decoder_tele_radio_remote_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_tele_radio_remote_free(void* context);
void subghz_protocol_decoder_tele_radio_remote_reset(void* context);
void subghz_protocol_decoder_tele_radio_remote_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_tele_radio_remote_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_tele_radio_remote_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus subghz_protocol_decoder_tele_radio_remote_deserialize(
    void* context,
    FlipperFormat* flipper_format);
void subghz_protocol_decoder_tele_radio_remote_get_string(void* context, FuriString* output);
