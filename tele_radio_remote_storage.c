#include "tele_radio_remote_storage.h"

#include <flipper_format.h>
#include <stdlib.h>
#include <storage/storage.h>
#include <string.h>
#include <toolbox/path.h>

#include "tele_radio_remote_core.h"
#include "tele_radio_remote_radio.h"

#define TRR_EXPORT_DIR EXT_PATH("export")

static void tele_radio_remote_storage_ensure_dir(Storage* storage) {
    storage_common_mkdir(storage, EXT_PATH("subghz"));
    storage_common_mkdir(storage, TRR_STORAGE_DIR);
}

/* Create a recoverable backup before truncating a confirmed replacement.
 * Never replace a backup left by an interrupted write. */
static bool copy_key(Storage* storage, const char* path, const char* to, FS_OpenMode mode) {
    File* source = storage_file_alloc(storage);
    File* dest = storage_file_alloc(storage);
    bool opened = false;
    bool ok = false;
    if(storage_file_open(source, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        uint64_t size = storage_file_size(source);
        if(size <= SIZE_MAX && storage_file_get_error(source) == FSE_OK &&
           (opened = storage_file_open(dest, to, FSAM_WRITE, mode))) {
            ok = storage_file_copy_to_file(source, dest, (size_t)size) && storage_file_sync(dest);
        }
    }
    ok = storage_file_close(dest) && ok;
    storage_file_close(source);
    storage_file_free(dest);
    storage_file_free(source);
    if(opened && !ok && mode == FSOM_CREATE_NEW) storage_common_remove(storage, to);
    return ok;
}

static void key_path(uint64_t key, char* buf, size_t size) {
    char name[32];
    trr_make_key_filename(name, sizeof(name), trr_key_id(key), trr_key_button(key));
    snprintf(buf, size, "%s/%s", TRR_STORAGE_DIR, name);
}

void tele_radio_remote_storage_backup_path(uint64_t key, char* buf, size_t size) {
    char path[96];
    key_path(key, path, sizeof(path));
    snprintf(buf, size, "%s.bak", path);
}

bool tele_radio_remote_storage_remove_backup(uint64_t key) {
    char backup[112];
    tele_radio_remote_storage_backup_path(key, backup, sizeof(backup));
    Storage* storage = furi_record_open(RECORD_STORAGE);
    bool gone = storage_simply_remove(storage, backup);
    furi_record_close(RECORD_STORAGE);
    return gone;
}

bool tele_radio_remote_storage_delete_key(uint64_t key) {
    char path[96], backup[112];
    key_path(key, path, sizeof(path));
    tele_radio_remote_storage_backup_path(key, backup, sizeof(backup));
    Storage* storage = furi_record_open(RECORD_STORAGE);
    bool gone = storage_simply_remove(storage, path) && storage_simply_remove(storage, backup);
    furi_record_close(RECORD_STORAGE);
    return gone;
}

bool tele_radio_remote_storage_export_key(uint64_t key, char* out_path, size_t out_size) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    bool ok = false;
    if(storage_simply_mkdir(storage, TRR_EXPORT_DIR)) {
        char path[96], dst[112];
        key_path(key, path, sizeof(path));
        FuriString* stem = furi_string_alloc();
        FuriString* name = furi_string_alloc();
        path_extract_filename_no_ext(path, stem);
        storage_get_next_filename(
            storage, TRR_EXPORT_DIR, furi_string_get_cstr(stem), TRR_KEY_FILE_EXTENSION, name, 32);
        snprintf(
            dst, sizeof(dst), "%s/%s%s", TRR_EXPORT_DIR, furi_string_get_cstr(name),
            TRR_KEY_FILE_EXTENSION);
        furi_string_free(name);
        furi_string_free(stem);

        ok = copy_key(storage, path, dst, FSOM_CREATE_NEW);
        if(ok && out_path && out_size) snprintf(out_path, out_size, "%s", dst);
    }
    furi_record_close(RECORD_STORAGE);
    return ok;
}

TRRSaveResult tele_radio_remote_storage_save_key(uint64_t key, uint32_t freq_hz, bool replace) {
    char path[96];
    key_path(key, path, sizeof(path));
    char backup[112];
    tele_radio_remote_storage_backup_path(key, backup, sizeof(backup));

    Storage* storage = furi_record_open(RECORD_STORAGE);
    if(replace && storage_file_exists(storage, backup)) {
        furi_record_close(RECORD_STORAGE);
        return TRRSaveBackupExists;
    }
    tele_radio_remote_storage_ensure_dir(storage);
    FlipperFormat* fff = flipper_format_file_alloc(storage);
    bool opened = false;
    bool backup_created = false;
    bool ok = false;
    do {
        if(replace && !(backup_created = copy_key(storage, path, backup, FSOM_CREATE_NEW)))
            break;
        opened = replace ? flipper_format_file_open_always(fff, path) :
                           flipper_format_file_open_new(fff, path);
        if(!opened)
            break;
        uint32_t version = TRR_KEY_FILE_VERSION;
        uint32_t freq = freq_hz;
        uint32_t bit = TRR_KEY_BITS;
        uint8_t key_bytes[8];
        for(int i = 0; i < 8; i++)
            key_bytes[i] = (uint8_t)(key >> (56 - 8 * i));
        if(!flipper_format_write_header_cstr(fff, TRR_KEY_FILE_TYPE, version))
            break;
        if(!flipper_format_write_uint32(fff, "Frequency", &freq, 1))
            break;
        if(!flipper_format_write_string_cstr(fff, "Preset", "FuriHalSubGhzPresetCustom"))
            break;
        if(!flipper_format_write_string_cstr(fff, "Custom_preset_module", "CC1101"))
            break;
        if(!flipper_format_write_hex(fff, "Custom_preset_data", trr_cc1101_preset, sizeof(trr_cc1101_preset)))
            break;
        if(!flipper_format_write_string_cstr(fff, "Protocol", TRR_PROTOCOL_NAME))
            break;
        if(!flipper_format_write_uint32(fff, "Bit", &bit, 1))
            break;
        if(!flipper_format_write_hex(fff, "Key", key_bytes, sizeof(key_bytes)))
            break;
        ok = true;
    } while(false);
    ok = flipper_format_file_close(fff) && ok;
    flipper_format_free(fff);
    if(replace) {
        if(!opened) {
            /* drop a stale backup so .bak can never block future replacements. */
            if(backup_created)
                storage_common_remove(storage, backup);
        } else if(ok || copy_key(storage, backup, path, FSOM_CREATE_ALWAYS)) {
            storage_common_remove(storage, backup);
        }
    }
    if(!ok && opened && !replace) storage_common_remove(storage, path);
    furi_record_close(RECORD_STORAGE);
    return ok ? TRRSaveOk : TRRSaveFailed;
}

bool tele_radio_remote_storage_key_exists(uint64_t key) {
    char path[96];
    key_path(key, path, sizeof(path));
    Storage* storage = furi_record_open(RECORD_STORAGE);
    bool exists = storage_file_exists(storage, path);
    furi_record_close(RECORD_STORAGE);
    return exists;
}

bool tele_radio_remote_storage_load_key(const char* name, uint64_t* key_out, uint32_t* freq_out) {
    uint32_t id;
    uint8_t button;
    if(!key_out || !trr_parse_key_filename(name, &id, &button)) return false;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    char path[128];
    snprintf(path, sizeof(path), "%s/%s", TRR_STORAGE_DIR, name);
    FlipperFormat* fff = flipper_format_file_alloc(storage);
    FuriString* field = furi_string_alloc();
    bool ok = false;
    do {
        if(!flipper_format_file_open_existing(fff, path))
            break;
        uint32_t bit = 0;
        uint32_t freq = TRR_FREQUENCY_HZ; /* default for key files without Frequency */
        uint8_t key_bytes[8] = {0};
        uint32_t version;
        if(!flipper_format_read_header(fff, field, &version) ||
           !furi_string_equal(field, TRR_KEY_FILE_TYPE) || version != TRR_KEY_FILE_VERSION)
            break;
        if(flipper_format_key_exist(fff, "Frequency") &&
           (!flipper_format_read_uint32(fff, "Frequency", &freq, 1) ||
            (freq != TRR_FREQUENCY_HZ && freq != TRR_FREQUENCY_433_HZ)))
            break;
        if(!flipper_format_rewind(fff) || !flipper_format_read_string(fff, "Protocol", field) ||
           !furi_string_equal(field, TRR_PROTOCOL_NAME)) break;
        if(!flipper_format_read_uint32(fff, "Bit", &bit, 1))
            break;
        if(bit != TRR_KEY_BITS)
            break;
        if(!flipper_format_read_hex(fff, "Key", key_bytes, sizeof(key_bytes)))
            break;
        uint64_t key = 0;
        for(int i = 0; i < 8; i++)
            key |= (uint64_t)key_bytes[i] << (56 - 8 * i);
        if(key != trr_make_key(id, button)) break;
        *key_out = key;
        if(freq_out)
            *freq_out = freq;
        ok = true;
    } while(false);
    furi_string_free(field);
    flipper_format_file_close(fff);
    flipper_format_free(fff);
    furi_record_close(RECORD_STORAGE);
    return ok;
}

/* qsort is disabled for FAPs. */
static void sort_u32(uint32_t* values, size_t count) {
    for(size_t i = 1; i < count; i++) {
        uint32_t v = values[i];
        size_t j = i;
        while(j > 0 && values[j - 1] > v) {
            values[j] = values[j - 1];
            j--;
        }
        values[j] = v;
    }
}

static size_t collect_id_page(
    File* dir,
    uint32_t first_id,
    uint32_t* ids,
    size_t limit,
    bool* more) {
    size_t count = 0;
    *more = false;
    if(storage_dir_open(dir, TRR_STORAGE_DIR)) {
        FileInfo fi;
        char name[64];
        while(storage_dir_read(dir, &fi, name, sizeof(name))) {
            uint32_t id;
            if(file_info_is_dir(&fi) || !trr_parse_key_filename(name, &id, NULL) || id < first_id)
                continue;

            size_t pos = 0;
            size_t end = count;
            while(pos < end) {
                size_t mid = pos + (end - pos) / 2;
                if(ids[mid] < id) pos = mid + 1;
                else end = mid;
            }
            if(pos < count && ids[pos] == id) continue;
            if(count == limit) {
                *more = true;
                if(pos == limit) continue;
                count--;
            }
            memmove(ids + pos + 1, ids + pos, (count - pos) * sizeof(*ids));
            ids[pos] = id;
            count++;
        }
    }
    storage_dir_close(dir);
    return count;
}

size_t tele_radio_remote_storage_list_ids(size_t limit, uint32_t** ids_out) {
    *ids_out = NULL;
    if(limit == 0) return 0;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* dir = storage_file_alloc(storage);
    uint32_t* ids = malloc(limit * sizeof(*ids));
    bool more;
    size_t total = collect_id_page(dir, 0, ids, limit, &more);
    if(more) {
        uint32_t first_id = ids[total - 1] + 1;
        uint32_t* page = malloc(limit * sizeof(*page));
        do {
            size_t count = collect_id_page(dir, first_id, page, limit, &more);
            total += count;
            if(more) first_id = page[count - 1] + 1;
        } while(more);
        free(page);
    }
    storage_file_free(dir);
    furi_record_close(RECORD_STORAGE);
    *ids_out = ids;
    return total;
}

size_t tele_radio_remote_storage_list_buttons(uint32_t id24, uint32_t** btns_out) {
    uint32_t* values = NULL;
    size_t count = 0;
    size_t capacity = 0;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* dir = storage_file_alloc(storage);
    if(storage_dir_open(dir, TRR_STORAGE_DIR)) {
        FileInfo fi;
        char name[64];
        while(storage_dir_read(dir, &fi, name, sizeof(name))) {
            uint32_t id;
            uint8_t btn;
            if(file_info_is_dir(&fi) || !trr_parse_key_filename(name, &id, &btn)) continue;
            if(id != (id24 & 0xFFFFFFu)) continue;
            bool seen = false;
            for(size_t i = 0; i < count && !seen; i++)
                seen = values[i] == btn;
            if(seen) continue;
            if(count == capacity) {
                capacity = capacity ? capacity * 2 : 16;
                values = realloc(values, capacity * sizeof(*values)); /* furi crashes on OOM */
            }
            values[count++] = btn;
        }
        storage_dir_close(dir);
    }
    storage_file_free(dir);
    furi_record_close(RECORD_STORAGE);
    if(count > 1) sort_u32(values, count);
    *btns_out = values;
    return count;
}
