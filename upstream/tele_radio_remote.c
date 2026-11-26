#include "tele_radio_remote.h"
#include "tele_radio_remote_core.h"

#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

#include <furi.h>

#define TAG "SubGhzProtocolTeleRadioRemote"

struct SubGhzProtocolDecoderTeleRadioRemote {
    SubGhzProtocolDecoderBase base;
    TRRDecoder core;
    SubGhzBlockGeneric generic;
};

struct SubGhzProtocolEncoderTeleRadioRemote {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
    LevelDuration joined; /* stop-bit run + next frame's idle run */
};

const SubGhzProtocolDecoder subghz_protocol_tele_radio_remote_decoder = {
    .alloc = subghz_protocol_decoder_tele_radio_remote_alloc,
    .free = subghz_protocol_decoder_tele_radio_remote_free,
    .feed = subghz_protocol_decoder_tele_radio_remote_feed,
    .reset = subghz_protocol_decoder_tele_radio_remote_reset,
    .get_hash_data = subghz_protocol_decoder_tele_radio_remote_get_hash_data,
    .serialize = subghz_protocol_decoder_tele_radio_remote_serialize,
    .deserialize = subghz_protocol_decoder_tele_radio_remote_deserialize,
    .get_string = subghz_protocol_decoder_tele_radio_remote_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_tele_radio_remote_encoder = {
    .alloc = subghz_protocol_encoder_tele_radio_remote_alloc,
    .free = subghz_protocol_encoder_tele_radio_remote_free,
    .deserialize = subghz_protocol_encoder_tele_radio_remote_deserialize,
    .stop = subghz_protocol_encoder_tele_radio_remote_stop,
    .yield = subghz_protocol_encoder_tele_radio_remote_yield,
};

const SubGhzProtocol subghz_protocol_tele_radio_remote = {
    .name = SUBGHZ_PROTOCOL_TELE_RADIO_REMOTE_NAME,
    .type = SubGhzProtocolTypeStatic,
    .flag = SubGhzProtocolFlag_868 | SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM |
            SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_tele_radio_remote_decoder,
    .encoder = &subghz_protocol_tele_radio_remote_encoder,
};

/* ------------------------------ encoder ------------------------------ */

void* subghz_protocol_encoder_tele_radio_remote_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderTeleRadioRemote* instance = malloc(sizeof(SubGhzProtocolEncoderTeleRadioRemote));
    instance->base.protocol = &subghz_protocol_tele_radio_remote;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = TRR_TX_REPEATS; /* default until deserialize() runs */
    instance->encoder.size_upload = TRR_FRAME_CELLS; /* upper bound, runs merged */
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void subghz_protocol_encoder_tele_radio_remote_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderTeleRadioRemote* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

static bool subghz_protocol_encoder_tele_radio_remote_get_upload(SubGhzProtocolEncoderTeleRadioRemote* instance) {
    uint8_t cells[TRR_FRAME_CELLS];
    trr_build_frame_cells(instance->generic.data, cells);
    size_t index = 0, i = 0;
    while(i < TRR_FRAME_CELLS) {
        size_t j = i;
        while(j < TRR_FRAME_CELLS && cells[j] == cells[i])
            j++;
        instance->encoder.upload[index++] =
            level_duration_make(cells[i] != 0, (uint32_t)(j - i) * TRR_TE_US);
        i = j;
    }
    instance->encoder.size_upload = index;
    /* Frames start with idle HIGH and end with a stop bit (HIGH).
     * Repeats are joined into one HIGH run so the carrier is continuous. */
    instance->joined = level_duration_make(
        true,
        level_duration_get_duration(instance->encoder.upload[index - 1]) +
            level_duration_get_duration(instance->encoder.upload[0]));
    return true;
}

SubGhzProtocolStatus subghz_protocol_encoder_tele_radio_remote_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolEncoderTeleRadioRemote* instance = context;
    /* A reused instance may have stopped mid-buffer. A failed load must
     * leave it stopped, and no cursor may survive a change of upload size. */
    instance->encoder.is_running = false;
    instance->encoder.front = 0;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;
    do {
        ret = subghz_block_generic_deserialize(&instance->generic, flipper_format);
        if(ret != SubGhzProtocolStatusOk)
            break;
        if(instance->generic.data_count_bit != TRR_KEY_BITS) {
            FURI_LOG_E(TAG, "Wrong number of bits in key");
            ret = SubGhzProtocolStatusErrorValueBitCount;
            break;
        }
        uint32_t repeats = TRR_TX_REPEATS;
        flipper_format_read_uint32(flipper_format, "Repeat", &repeats, 1);
        instance->encoder.repeat = repeats;
        if(!subghz_protocol_encoder_tele_radio_remote_get_upload(instance)) {
            ret = SubGhzProtocolStatusErrorEncoderGetUpload;
            break;
        }
        instance->encoder.is_running = true;
    } while(false);
    return ret;
}

void subghz_protocol_encoder_tele_radio_remote_stop(void* context) {
    SubGhzProtocolEncoderTeleRadioRemote* instance = context;
    instance->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_tele_radio_remote_yield(void* context) {
    SubGhzProtocolEncoderTeleRadioRemote* instance = context;
    if(instance->encoder.repeat == 0 || !instance->encoder.is_running) {
        instance->encoder.is_running = false;
        return level_duration_reset();
    }
    size_t last = instance->encoder.size_upload - 1;
    if(instance->encoder.front == last && instance->encoder.repeat > 1) {
        instance->encoder.repeat--;
        instance->encoder.front = 1; /* run 0 is inside the joined run */
        return instance->joined;
    }
    LevelDuration ret = instance->encoder.upload[instance->encoder.front];
    if(++instance->encoder.front == instance->encoder.size_upload) {
        instance->encoder.repeat--;
        instance->encoder.front = 0;
    }
    return ret;
}

/* ------------------------------ decoder ------------------------------ */

void* subghz_protocol_decoder_tele_radio_remote_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderTeleRadioRemote* instance = malloc(sizeof(SubGhzProtocolDecoderTeleRadioRemote));
    instance->base.protocol = &subghz_protocol_tele_radio_remote;
    instance->generic.protocol_name = instance->base.protocol->name;
    trr_decoder_init(&instance->core);
    return instance;
}

void subghz_protocol_decoder_tele_radio_remote_free(void* context) {
    furi_assert(context);
    free(context);
}

void subghz_protocol_decoder_tele_radio_remote_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderTeleRadioRemote* instance = context;
    trr_decoder_init(&instance->core);
}

void subghz_protocol_decoder_tele_radio_remote_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderTeleRadioRemote* instance = context;
    uint32_t cells = (duration + TRR_TE_US / 2) / TRR_TE_US;
    if(cells == 0)
        cells = 1;
    if(cells > 32)
        cells = 32;
    for(uint32_t i = 0; i < cells; i++)
        trr_decoder_feed_cell(&instance->core, level ? 1 : 0);
    uint64_t key;
    if(trr_decoder_poll(&instance->core, &key)) {
        instance->generic.data = key;
        instance->generic.data_count_bit = TRR_KEY_BITS;
        if(instance->base.callback)
            instance->base.callback(&instance->base, instance->base.context);
    }
}

uint8_t subghz_protocol_decoder_tele_radio_remote_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderTeleRadioRemote* instance = context;
    uint64_t d = instance->generic.data;
    return (uint8_t)(d ^ (d >> 8) ^ (d >> 16) ^ (d >> 24) ^ (d >> 32));
}

SubGhzProtocolStatus subghz_protocol_decoder_tele_radio_remote_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderTeleRadioRemote* instance = context;
    return subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus subghz_protocol_decoder_tele_radio_remote_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderTeleRadioRemote* instance = context;
    SubGhzProtocolStatus ret =
        subghz_block_generic_deserialize_check_count_bit(&instance->generic, flipper_format, TRR_KEY_BITS);
    return ret;
}

void subghz_protocol_decoder_tele_radio_remote_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderTeleRadioRemote* instance = context;
    furi_string_cat_printf(
        output,
        "%s 32bit\r\n"
        "Key:0x%08llX\r\n"
        "ID:%06lX BTN:%02X\r\n",
        instance->generic.protocol_name,
        (unsigned long long)instance->generic.data,
        (unsigned long)trr_key_id(instance->generic.data),
        trr_key_button(instance->generic.data));
}
