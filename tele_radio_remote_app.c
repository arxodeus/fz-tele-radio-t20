#include <furi.h>
#include <flipper_format.h>
#include <gui/modules/dialog_ex.h>
#include <gui/modules/submenu.h>
#include <gui/modules/variable_item_list.h>
#include <gui/view_holder.h>
#include <gui/view_stack.h>
#include <stdatomic.h>
#include <storage/storage.h>

#include "tele_radio_remote_core.h"
#include "tele_radio_remote_radio.h"
#include "tele_radio_remote_storage.h"

#define RX_RING_SIZE 512
/* How many before needing to go to next page. */
#define MAX_MENU_REMOTES 200
#define MORE_INDEX       UINT32_MAX
#define BUTTON_ROWS      3u
#define BUTTON_STATUS_MS 2000u

/* Settings saved in a FlipperFormat file under apps_data. */
#define APP_DATA_DIR     EXT_PATH("apps_data/tele_radio_remote")
#define SETTINGS_PATH    APP_DATA_DIR "/settings.cfg"
#define SETTINGS_HEADER  "Flipper Tele Radio Remote Settings"
#define SETTINGS_VERSION 1

/* The frequency bands in ascending order. */
static const uint32_t band_options[] = {TRR_FREQUENCY_433_HZ, TRR_FREQUENCY_HZ};
static const char* const band_labels[2] = {"433", "869"};
#define BAND_COUNT   COUNT_OF(band_options)
#define BAND_DEFAULT TRR_FREQUENCY_HZ

static size_t band_index(uint32_t freq_hz) {
    return freq_hz == TRR_FREQUENCY_433_HZ ? 0 : 1;
}

/* Frames per send, first 1 to 10 by 1, then 10 to 100 by 10. */
static const uint8_t repeat_options[] =
    {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100};
#define REPEAT_OPTIONS_COUNT COUNT_OF(repeat_options)
#define REPEAT_DEFAULT       TRR_TX_REPEATS

static size_t repeat_index(uint32_t v) {
    size_t best = 0;
    for(size_t i = 0; i < REPEAT_OPTIONS_COUNT; i++) {
        if(repeat_options[i] == v) return i;
        if(repeat_options[i] < v) best = i;
    }
    return best;
}

typedef enum {
    Home,
    Buttons,
    Recording,
    Confirm,
    Result,
    Settings,
    Backup,
    DeleteKey
} Screen;
typedef enum {
    Back,
    Select,
    Send,
    Export,
    Accept,
    Discard,
    Delete
} Action;
typedef struct {
    Action action;
    uint32_t index;
} Event;

typedef struct {
    ViewHolder* holder;
    Submenu* menu;
    VariableItemList* settings;
    DialogEx* dialog;
    DialogEx* buttons_dialog;
    ViewStack* buttons_stack;
    View* buttons_input;
    FuriMessageQueue* events;
    Screen screen;
    Screen back_screen;
    uint32_t selected;
    uint32_t cur_id;
    uint8_t button_selected;
    uint32_t* buttons;
    size_t button_count;
    atomic_uint button_index;
    char buttons_status[32];
    bool button_status;
    uint32_t button_status_tick;
    size_t hidden_remotes;
    atomic_uint freq_hz, tx_repeats;
    atomic_bool settings_dirty;
    uint64_t pending_key;
    char dialog_text[128];
    TRRDecoder decoder;
    atomic_uint rx_head, rx_tail;
    uint8_t rx_ring[RX_RING_SIZE];
    atomic_bool busy, ok_down, was_tap, cancel;
    atomic_uint send_count;   /* button pushes */
    atomic_uint frames_total; /* frames sent */
    atomic_uint press_tick, hold_ms, last_release_tick; /* timing (ticks) */
    bool tracking_press; /* GUI input only */
    InputKey shortcut_key;
    uint8_t shortcut_button;
} App;

_Static_assert(__atomic_always_lock_free(sizeof(atomic_uint), 0),
               "RX ring indices must be lock-free in the ISR");

static bool post(App* app, Action action, uint32_t index) {
    Event event = {.action = action, .index = index};
    return furi_message_queue_put(app->events, &event, 0) == FuriStatusOk;
}

static void back_cb(void* ctx) {
    App* app = ctx;
    atomic_store(&app->cancel, true);
    post(app, Back, 0);
}

static void home_cb(void* ctx, uint32_t index) {
    post(ctx, Select, index);
}

static void dialog_cb(DialogExResult result, void* ctx) {
    App* app = ctx;
    if(result == DialogExResultLeft) atomic_store(&app->cancel, true);
    post(app,
         result == DialogExResultCenter ? Accept :
         result == DialogExResultRight  ? Delete :
                                          Discard,
         0);
}

static void detach(App* app) {
    view_holder_set_view(app->holder, NULL);
    furi_message_queue_reset(app->events);
    atomic_store(&app->busy, false);
}

static void show_dialog(
    App* app,
    Screen screen,
    const char* title,
    const char* text,
    const char* center,
    const char* left,
    const char* right) {
    detach(app);
    dialog_ex_set_header(app->dialog, title, 64, 3, AlignCenter, AlignTop);
    dialog_ex_set_text(app->dialog, text, 64, 30, AlignCenter, AlignCenter);
    dialog_ex_set_center_button_text(app->dialog, center);
    dialog_ex_set_left_button_text(app->dialog, left);
    dialog_ex_set_right_button_text(app->dialog, right);
    app->screen = screen;
    view_holder_set_view(app->holder, dialog_ex_get_view(app->dialog));
}

static void show_message(
    App* app,
    Screen screen,
    const char* title,
    const char* text,
    const char* button,
    const char* left) {
    show_dialog(app, screen, title, text, button, left, NULL);
}

static void show_home(App* app) {
    detach(app);
    free(app->buttons);
    app->buttons = NULL;
    app->button_count = 0;
    submenu_reset(app->menu);
    submenu_set_header(app->menu, "Tele Radio T20");
    submenu_add_item(app->menu, "Record", 0, home_cb, app);
    submenu_add_item(app->menu, "Settings", 1, home_cb, app);
    uint32_t* ids = NULL;
    size_t count = tele_radio_remote_storage_list_ids(MAX_MENU_REMOTES, &ids);
    char label[32];
    for(size_t i = 0; i < count && i < MAX_MENU_REMOTES; i++) {
        snprintf(label, sizeof(label), "Remote %06lX", (unsigned long)ids[i]);
        submenu_add_item(app->menu, label, ids[i] + 2, home_cb, app);
    }
    free(ids);
    app->hidden_remotes = count > MAX_MENU_REMOTES ? count - MAX_MENU_REMOTES : 0;
    if(app->hidden_remotes) {
        snprintf(label, sizeof(label), "+%u more not shown", (unsigned)app->hidden_remotes);
        submenu_add_item(app->menu, label, MORE_INDEX, home_cb, app);
    }
    submenu_set_selected_item(app->menu, app->selected);
    app->screen = Home;
    app->back_screen = Home;
    view_holder_set_view(app->holder, submenu_get_view(app->menu));
}

static void send_input(App* app, InputType type) {
    if(type == InputTypePress) {
        if(atomic_load(&app->cancel)) return;
        app->tracking_press = !atomic_exchange(&app->busy, true);
        if(!app->tracking_press) return;

        uint32_t now = furi_get_tick();
        if(now - atomic_load(&app->last_release_tick) > furi_ms_to_ticks(1000)) {
            atomic_store(&app->send_count, 0);
            atomic_store(&app->frames_total, 0);
        }
        atomic_store(&app->was_tap, true);
        atomic_store(&app->ok_down, true);
        atomic_store(&app->press_tick, now);
        atomic_store(&app->hold_ms, 0);
        uint32_t button = app->buttons[atomic_load(&app->button_index)];
        if(!post(app, Send, button)) {
            app->tracking_press = false;
            atomic_store(&app->ok_down, false);
            atomic_store(&app->busy, false);
        }
    } else if(app->tracking_press) {
        if(type == InputTypeLong) atomic_store(&app->was_tap, false);
        if(type == InputTypeRelease) {
            uint32_t now = furi_get_tick();
            atomic_store(&app->hold_ms, now - atomic_load(&app->press_tick));
            atomic_store(&app->last_release_tick, now);
            atomic_store(&app->ok_down, false);
            app->tracking_press = false;
        }
    }
}

static bool buttons_input_cb(InputEvent* event, void* ctx) {
    App* app = ctx;
    if(event->key == InputKeyOk) {
        send_input(app, event->type);
        return true;
    }
    bool sending = app->tracking_press || atomic_load(&app->busy);
    bool ready = !sending && !atomic_load(&app->cancel);
    if(event->key == InputKeyLeft || event->key == InputKeyRight) {
        if(event->type == InputTypePress && ready) {
            app->shortcut_key = event->key;
            app->shortcut_button = app->buttons[atomic_load(&app->button_index)];
        } else if(event->key == app->shortcut_key) {
            if(event->type == InputTypeShort && ready)
                post(app, event->key == InputKeyLeft ? Export : Delete, app->shortcut_button);
            if(event->type == InputTypeRelease) app->shortcut_key = InputKeyMAX;
        }
        return true;
    }
    if(event->key == InputKeyUp || event->key == InputKeyDown) {
        if(ready && (event->type == InputTypeShort || event->type == InputTypeRepeat) &&
           app->button_count > 1) {
            size_t index = atomic_load(&app->button_index);
            if(event->key == InputKeyUp)
                index = index ? index - 1 : app->button_count - 1;
            else
                index = index + 1 == app->button_count ? 0 : index + 1;
            atomic_store(&app->button_index, index);
            post(app, Select, 0);
        }
        return true;
    }
    return false;
}

static void render_buttons(App* app) {
    char text[80];
    size_t used = snprintf(text, sizeof(text), "%s", app->buttons_status);
    size_t selected = atomic_load(&app->button_index);
    size_t start = selected < BUTTON_ROWS ? 0 : selected - BUTTON_ROWS + 1;
    for(size_t i = start; i < app->button_count && i < start + BUTTON_ROWS; i++) {
        used += snprintf(
            text + used, sizeof(text) - used, "\n%c Button %02X", i == selected ? '>' : ' ',
            (unsigned int)(uint8_t)app->buttons[i]);
    }
    dialog_ex_set_text(app->buttons_dialog, text, 2, 10, AlignLeft, AlignTop);
}

static void set_buttons_status(App* app, const char* text) {
    if(strcmp(app->buttons_status, text) == 0) return;
    snprintf(app->buttons_status, sizeof(app->buttons_status), "%s", text);
    render_buttons(app);
}

static void show_buttons(App* app) {
    detach(app);
    free(app->buttons);
    app->button_count = tele_radio_remote_storage_list_buttons(app->cur_id, &app->buttons);
    if(app->button_count == 0) {
        show_home(app);
        return;
    }

    size_t selected = 0;
    while(selected + 1 < app->button_count && app->buttons[selected] < app->button_selected)
        selected++;
    app->button_selected = app->buttons[selected];
    atomic_store(&app->button_index, selected);
    app->buttons_status[0] = '\0';
    char title[24];
    snprintf(title, sizeof(title), "Remote %06lX", (unsigned long)app->cur_id);
    dialog_ex_set_header(app->buttons_dialog, title, 2, 1, AlignLeft, AlignTop);
    render_buttons(app);

    app->screen = Buttons;
    app->button_status = false;
    app->shortcut_key = InputKeyMAX;
    atomic_store(&app->cancel, false);
    atomic_store(&app->send_count, 0);
    atomic_store(&app->frames_total, 0);
    atomic_store(&app->hold_ms, 0);
    atomic_store(&app->last_release_tick, furi_get_tick());
    view_holder_set_view(app->holder, view_stack_get_view(app->buttons_stack));
}

static void export_button(App* app, uint8_t button) {
    app->button_selected = button;
    char path[112];
    if(tele_radio_remote_storage_export_key(trr_make_key(app->cur_id, button), path, sizeof(path))) {
        snprintf(app->dialog_text, sizeof(app->dialog_text), "Exported to\n%s", path);
        show_message(app, Result, "Exported", app->dialog_text, "Done", NULL);
    } else {
        show_message(app, Result, "Export failed", "Check the SD card.", "Done", NULL);
    }
    app->back_screen = Buttons;
}

static void confirm_delete(App* app, uint8_t button) {
    app->button_selected = button;
    char name[32];
    trr_make_key_filename(name, sizeof(name), app->cur_id, button);
    snprintf(app->dialog_text, sizeof(app->dialog_text), "%s\nDelete?", name);
    show_dialog(app, DeleteKey, "Delete key file", app->dialog_text, NULL, "Cancel", "Delete");
}

/* ISR context, only push quantized cells into the ring buffer */
static void rx_edge_cb(bool level, uint32_t duration_us, void* ctx) {
    App* app = ctx;
    uint32_t cells = (duration_us + TRR_TE_US / 2) / TRR_TE_US;
    if(cells == 0) cells = 1;
    if(cells > 32) cells = 32;
    for(uint32_t i = 0; i < cells; i++) {
        size_t next = (app->rx_head + 1) % RX_RING_SIZE;
        if(next != app->rx_tail) {
            app->rx_ring[app->rx_head] = level ? 1 : 0;
            app->rx_head = next;
        }
    }
}

static void format_capture(App* app) {
    snprintf(
        app->dialog_text,
        sizeof(app->dialog_text),
        "ID %06lX   Button %02X",
        (unsigned long)trr_key_id(app->pending_key),
        trr_key_button(app->pending_key));
}

/* Show where the blocking backup is. */
static void show_backup(App* app) {
    char path[64];
    tele_radio_remote_storage_backup_path(app->pending_key, path, sizeof(path));
    const char* name = strrchr(path, '/');
    name = name ? name + 1 : path;
    snprintf(
        app->dialog_text,
        sizeof(app->dialog_text),
        "%.*s\n%s",
        (int)(name - path),
        path,
        name);
    show_dialog(app, Backup, "Old backup in the way", app->dialog_text, NULL, "Keep", "Delete");
}

static void save_capture(App* app, bool replace) {
    TRRSaveResult result =
        tele_radio_remote_storage_save_key(app->pending_key, app->freq_hz, replace);
    if(result == TRRSaveBackupExists) {
        show_backup(app);
        return;
    }
    bool saved = result == TRRSaveOk;
    format_capture(app);
    show_message(
        app,
        Result,
        saved ? "Saved" : "Save failed",
        saved ? app->dialog_text : "Check SD card and backups.",
        "Done",
        NULL);
}

static void drain_rx(App* app) {
    while(!atomic_load(&app->cancel) && app->rx_tail != app->rx_head) {
        uint8_t cell = app->rx_ring[app->rx_tail];
        app->rx_tail = (app->rx_tail + 1) % RX_RING_SIZE;
        trr_decoder_feed_cell(&app->decoder, cell);
        if(trr_decoder_poll(&app->decoder, &app->pending_key)) {
            tele_radio_remote_radio_rx_stop();
            bool exists = tele_radio_remote_storage_key_exists(app->pending_key);
            /* Finish pending input before saving. */
            view_holder_set_view(app->holder, NULL);
            if(atomic_load(&app->cancel)) {
                show_home(app);
            } else if(exists) {
                format_capture(app);
                show_message(
                    app, Confirm, "Replace saved button?", app->dialog_text, "Replace", "Discard");
            } else {
                save_capture(app, false);
            }
            break;
        }
    }
}

/* Check before TX starts and from the ISR at each frame boundary.
 * A tap sends at least Repeats frames, a hold keeps going until release. */
static bool tx_should_continue(uint32_t frames, void* ctx) {
    App* app = ctx;
    if(atomic_load(&app->cancel)) return false;
    if(frames == 0 || atomic_load(&app->ok_down)) return true;
    return atomic_load(&app->was_tap) && frames < atomic_load(&app->tx_repeats);
}

/* The frame counter is shown only from the 3rd button push, or when OK has
 * been held for more than 1s. */
static bool show_frame_counter(App* app) {
    if(atomic_load(&app->send_count) >= 3)
        return true;
    if(atomic_load(&app->ok_down) &&
       furi_get_tick() - atomic_load(&app->press_tick) > furi_ms_to_ticks(1000))
        return true;
    return atomic_load(&app->hold_ms) > furi_ms_to_ticks(1000);
}

static void tx_progress(uint32_t frames, void* ctx) {
    App* app = ctx;
    if(show_frame_counter(app)) {
        uint32_t total = atomic_load(&app->frames_total) + frames;
        char status[32];
        snprintf(status, sizeof(status), "Sending frames %lu", (unsigned long)total);
        set_buttons_status(app, status);
    } else {
        set_buttons_status(app, "Sending frames");
    }
}

static void show_button_status(App* app, const char* status) {
    set_buttons_status(app, status);
    app->button_status_tick = furi_get_tick();
    app->button_status = true;
}

static void send_button(App* app, uint8_t button) {
    if(atomic_load(&app->cancel)) return;
    char name[32];
    trr_make_key_filename(name, sizeof(name), app->cur_id, button);
    uint64_t key;
    uint32_t frequency;
    if(!tele_radio_remote_storage_load_key(name, &key, &frequency)) {
        show_button_status(app, "Cannot read key file");
        return;
    }
    uint32_t sent = 0;
    TRRTxResult result = tele_radio_remote_radio_tx_stream(
        key, frequency, TRR_TX_MAX_FRAMES, tx_should_continue, tx_progress, app, &sent);
    if(result == TRRTxCancelled) return;
    char status[32];
    if(result == TRRTxBlocked) {
        snprintf(status, sizeof(status), "TX blocked by region");
    } else if(result == TRRTxTimeout) {
        snprintf(status, sizeof(status), "Radio timed out");
    } else if(sent >= TRR_TX_MAX_FRAMES) {
        snprintf(status, sizeof(status), "Stopped: %us limit", (unsigned)TRR_TX_MAX_SECONDS);
    } else {
        atomic_fetch_add(&app->frames_total, sent);
        if(show_frame_counter(app))
            snprintf(
                status,
                sizeof(status),
                "Sent frames %lu",
                (unsigned long)atomic_load(&app->frames_total));
        else
            snprintf(status, sizeof(status), "Sent frames");
    }
    show_button_status(app, status);
}

static bool app_save_settings(App* app) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(storage, EXT_PATH("apps_data"));
    storage_common_mkdir(storage, APP_DATA_DIR);
    FlipperFormat* fff = flipper_format_file_alloc(storage);
    bool saved = false;
    do {
        if(!flipper_format_file_open_always(fff, SETTINGS_PATH)) break;
        uint32_t version = SETTINGS_VERSION;
        uint32_t band = app->freq_hz;
        uint32_t repeats = app->tx_repeats;
        if(!flipper_format_write_header_cstr(fff, SETTINGS_HEADER, version)) break;
        saved = flipper_format_write_uint32(fff, "Band", &band, 1) &&
                flipper_format_write_uint32(fff, "Repeats", &repeats, 1);
    } while(false);
    saved = flipper_format_file_close(fff) && saved;
    flipper_format_free(fff);
    furi_record_close(RECORD_STORAGE);
    return saved;
}

static void app_load_settings(App* app) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* fff = flipper_format_file_alloc(storage);
    do {
        if(!flipper_format_file_open_existing(fff, SETTINGS_PATH)) break;
        FuriString* header = furi_string_alloc();
        uint32_t version = 0;
        bool ok = flipper_format_read_header(fff, header, &version) &&
                  version == SETTINGS_VERSION && furi_string_equal(header, SETTINGS_HEADER);
        furi_string_free(header);
        if(!ok) break;
        uint32_t band = 0;
        if(flipper_format_read_uint32(fff, "Band", &band, 1) &&
           (band == TRR_FREQUENCY_HZ || band == TRR_FREQUENCY_433_HZ))
            app->freq_hz = band; /* accept only known bands, keep default 869 MHz */
        uint32_t repeats = 0;
        if(flipper_format_rewind(fff) && flipper_format_read_uint32(fff, "Repeats", &repeats, 1) &&
           repeats >= 1 && repeats <= 100)
            app->tx_repeats = repeat_options[repeat_index(repeats)];
    } while(false);
    flipper_format_file_close(fff);
    flipper_format_free(fff);
    furi_record_close(RECORD_STORAGE);
}

/* Settings are saved when leaving the Settings screen. */
static void band_changed(VariableItem* item) {
    App* app = variable_item_get_context(item);
    uint8_t i = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, band_labels[i]);
    if(app->freq_hz != band_options[i]) {
        app->freq_hz = band_options[i];
        app->settings_dirty = true;
    }
}

static void repeats_changed(VariableItem* item) {
    App* app = variable_item_get_context(item);
    uint32_t value = repeat_options[variable_item_get_current_value_index(item)];
    if(app->tx_repeats != value) {
        app->tx_repeats = value;
        app->settings_dirty = true;
    }
    char text[12];
    snprintf(text, sizeof(text), "%ux", (unsigned)app->tx_repeats);
    variable_item_set_current_value_text(item, text);
}

static void show_settings(App* app) {
    detach(app);
    variable_item_list_reset(app->settings);
    VariableItem* item =
        variable_item_list_add(app->settings, "Band (MHz)", BAND_COUNT, band_changed, app);
    variable_item_set_current_value_index(item, band_index(app->freq_hz));
    band_changed(item);
    item = variable_item_list_add(
        app->settings, "Frames/send", REPEAT_OPTIONS_COUNT, repeats_changed, app);
    variable_item_set_current_value_index(item, repeat_index(app->tx_repeats));
    repeats_changed(item);
    app->screen = Settings;
    view_holder_set_view(app->holder, variable_item_list_get_view(app->settings));
}

static void select_home_item(App* app, uint32_t index) {
    app->selected = index;
    if(index == 0) {
        atomic_store(&app->cancel, false);
        snprintf(
            app->dialog_text,
            sizeof(app->dialog_text),
            "%s MHz",
            band_labels[band_index(app->freq_hz)]);
        show_message(app, Recording, "Listening", app->dialog_text, NULL, "Cancel");
        trr_decoder_init(&app->decoder);
        app->rx_head = app->rx_tail = 0;
        tele_radio_remote_radio_rx_start(rx_edge_cb, app, app->freq_hz);
    } else if(index == 1) {
        show_settings(app);
    } else if(index == MORE_INDEX) {
        snprintf(
            app->dialog_text,
            sizeof(app->dialog_text),
            "Showing %u remotes.\n%u more are on the SD card.",
            (unsigned)MAX_MENU_REMOTES,
            (unsigned)app->hidden_remotes);
        show_message(app, Result, "Too many remotes", app->dialog_text, "OK", NULL);
    } else {
        if(app->cur_id != index - 2) app->button_selected = 0;
        app->cur_id = index - 2;
        show_buttons(app);
    }
}

static uint32_t event_timeout(App* app) {
    if(app->screen == Recording) return furi_ms_to_ticks(50);
    if(app->screen == Buttons && app->button_status) {
        uint32_t elapsed = furi_get_tick() - app->button_status_tick;
        uint32_t duration = furi_ms_to_ticks(BUTTON_STATUS_MS);
        if(elapsed < duration) return duration - elapsed;
        set_buttons_status(app, "");
        app->button_status = false;
    }
    return FuriWaitForever;
}

int32_t tele_radio_remote_app(void* context) {
    UNUSED(context);
    App* app = calloc(1, sizeof(App));
    app->events = furi_message_queue_alloc(16, sizeof(Event));
    app->freq_hz = BAND_DEFAULT;
    app->tx_repeats = REPEAT_DEFAULT;
    app_load_settings(app);
    app->menu = submenu_alloc();
    app->dialog = dialog_ex_alloc();
    dialog_ex_set_context(app->dialog, app);
    dialog_ex_set_result_callback(app->dialog, dialog_cb);
    app->buttons_dialog = dialog_ex_alloc();
    dialog_ex_set_left_button_text(app->buttons_dialog, "Export");
    dialog_ex_set_center_button_text(app->buttons_dialog, "Send");
    dialog_ex_set_right_button_text(app->buttons_dialog, "Delete");
    app->buttons_input = view_alloc();
    view_set_context(app->buttons_input, app);
    view_set_input_callback(app->buttons_input, buttons_input_cb);
    app->buttons_stack = view_stack_alloc();
    view_stack_add_view(app->buttons_stack, dialog_ex_get_view(app->buttons_dialog));
    view_stack_add_view(app->buttons_stack, app->buttons_input);
    app->settings = variable_item_list_alloc();
    app->holder = view_holder_alloc();
    Gui* gui = furi_record_open(RECORD_GUI);
    view_holder_attach_to_gui(app->holder, gui);
    view_holder_set_back_callback(app->holder, back_cb, app);
    tele_radio_remote_radio_init(app->freq_hz);
    show_home(app);

    bool running = true;
    while(running) {
        Event event;
        if(furi_message_queue_get(app->events, &event, event_timeout(app)) != FuriStatusOk) {
            if(app->screen == Recording) drain_rx(app);
            continue;
        }
        if(event.action == Back || event.action == Discard) {
            if(app->screen == Home) {
                running = false;
            } else {
                if(app->screen == Recording) tele_radio_remote_radio_rx_stop();
                if(app->screen == Settings) {
                    detach(app);
                    if(app->settings_dirty && !app_save_settings(app)) {
                        show_message(
                            app, Result, "Settings not saved", "Check the SD card.", "Done", NULL);
                        continue;
                    }
                    app->settings_dirty = false;
                }
                if(app->screen == Buttons) {
                    app->button_selected = app->buttons[atomic_load(&app->button_index)];
                    show_home(app);
                } else if(app->screen == DeleteKey ||
                          (app->screen == Result && app->back_screen == Buttons)) {
                    show_buttons(app);
                } else {
                    show_home(app);
                }
            }
        } else if(app->screen == Home && event.action == Select) {
            select_home_item(app, event.index);
        } else if(app->screen == Buttons && event.action == Select) {
            render_buttons(app);
        } else if(app->screen == Buttons && event.action == Send) {
            atomic_fetch_add(&app->send_count, 1);
            send_button(app, event.index);
            atomic_store(&app->busy, false);
        } else if(app->screen == Buttons && event.action == Export) {
            export_button(app, event.index);
        } else if(app->screen == Buttons && event.action == Delete) {
            confirm_delete(app, event.index);
        } else if(app->screen == DeleteKey && event.action == Delete) {
            uint8_t button = app->button_selected;
            bool ok = tele_radio_remote_storage_delete_key(trr_make_key(app->cur_id, button));
            char name[32];
            trr_make_key_filename(name, sizeof(name), app->cur_id, button);
            snprintf(app->dialog_text, sizeof(app->dialog_text), "%s", name);
            show_message(
                app,
                Result,
                ok ? "Deleted" : "Delete failed",
                ok ? app->dialog_text : "Check the SD card.",
                "Done",
                NULL);
            app->back_screen = Buttons;
        } else if(app->screen == Confirm && event.action == Accept) {
            save_capture(app, true);
        } else if(app->screen == Backup && event.action == Delete) {
            if(tele_radio_remote_storage_remove_backup(app->pending_key)) {
                save_capture(app, true);
            } else {
                show_message(
                    app, Result, "Delete failed", "Check the SD card.", "Done", NULL);
            }
        } else if(app->screen == Result && event.action == Accept) {
            if(app->back_screen == Buttons)
                show_buttons(app);
            else
                show_home(app);
        }
    }

    detach(app);
    view_holder_free(app->holder);
    submenu_free(app->menu);
    variable_item_list_free(app->settings);
    dialog_ex_free(app->dialog);
    view_stack_free(app->buttons_stack);
    view_free(app->buttons_input);
    dialog_ex_free(app->buttons_dialog);
    free(app->buttons);
    furi_record_close(RECORD_GUI);
    tele_radio_remote_radio_deinit();
    furi_message_queue_free(app->events);
    free(app);
    return 0;
}
