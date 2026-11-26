#pragma once

/* Saving/loading keys as standard Flipper SubGhz Key Files
 * (Protocol: TeleRadioRemote, Bit: 32, Key: <8 hex bytes>) under /ext/subghz/TeleRadioRemote */

#include <furi.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define TRR_STORAGE_DIR        EXT_PATH("subghz/TeleRadioRemote")
#define TRR_PROTOCOL_NAME      "TeleRadioRemote"
#define TRR_KEY_FILE_TYPE      "Flipper SubGhz Key File"
#define TRR_KEY_FILE_VERSION   1
#define TRR_KEY_FILE_EXTENSION ".sub"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TRRSaveOk,
    TRRSaveFailed,
    TRRSaveBackupExists,
} TRRSaveResult;

/* Save key under TRR_STORAGE_DIR with an auto-generated name (TRR_<ID>_B<n>.sub). */
TRRSaveResult tele_radio_remote_storage_save_key(uint64_t key, uint32_t freq_hz, bool replace);

/* Full path of the replacement backup for this key. */
void tele_radio_remote_storage_backup_path(uint64_t key, char* buf, size_t size);
/* Delete that backup. True if it is gone afterwards. */
bool tele_radio_remote_storage_remove_backup(uint64_t key);

/* Delete the key file for this ID+button. True if gone. */
bool tele_radio_remote_storage_delete_key(uint64_t key);

/* Copy the key file to /ext/export/ under an unused name.
 * out_path then receives the full destination path. */
bool tele_radio_remote_storage_export_key(uint64_t key, char* out_path, size_t out_size);

/* True if a key file for this ID+button already exists. */
bool tele_radio_remote_storage_key_exists(uint64_t key);

/* Load key from a file inside TRR_STORAGE_DIR. */
bool tele_radio_remote_storage_load_key(const char* name, uint64_t* key_out, uint32_t* freq_out);

/* Return the total number of distinct IDs, with up to limit IDs sorted in ids_out. */
size_t tele_radio_remote_storage_list_ids(size_t limit, uint32_t** ids_out);
/* Buttons saved for one remote ID. */
size_t tele_radio_remote_storage_list_buttons(uint32_t id24, uint32_t** btns_out);

#ifdef __cplusplus
}
#endif
