#include "ble_toolkit.h"

#include <furi_hal_random.h>
#include <string.h>

// Re-apply the payload for modes that should look like they're cycling
// through nearby devices, rather than sitting on one fixed advertisement.
#define BLE_TOOLKIT_TICK_PERIOD_MS 2000

// Rising two-note confirmation when broadcasting starts.
const NotificationSequence ble_toolkit_seq_start = {
    &message_green_255,
    &message_note_c5,
    &message_delay_50,
    &message_note_g5,
    &message_delay_50,
    &message_sound_off,
    &message_green_0,
    NULL,
};

// Falling two-note cue when broadcasting stops.
const NotificationSequence ble_toolkit_seq_stop = {
    &message_note_g4,
    &message_delay_50,
    &message_note_c4,
    &message_delay_50,
    &message_sound_off,
    NULL,
};

// AirTag "locate"-style chirp: two quick high beeps with a blue flash,
// emitted once per cycle while the AirTag emulator is broadcasting.
const NotificationSequence ble_toolkit_seq_airtag_chirp = {
    &message_blue_255,
    &message_note_a6,
    &message_delay_50,
    &message_note_e6,
    &message_delay_50,
    &message_sound_off,
    &message_blue_0,
    NULL,
};

static bool ble_toolkit_app_custom_event_callback(void* context, uint32_t event) {
    BleToolkitApp* app = context;
    return scene_manager_handle_custom_event(app->scene_manager, event);
}

static bool ble_toolkit_app_back_event_callback(void* context) {
    BleToolkitApp* app = context;
    return scene_manager_handle_back_event(app->scene_manager);
}

static void ble_toolkit_app_tick_event_callback(void* context) {
    BleToolkitApp* app = context;
    scene_manager_handle_tick_event(app->scene_manager);
}

void ble_toolkit_app_apply_payload(BleToolkitApp* app) {
    app->beacon_data_len = ble_toolkit_payload_build(app->mode, app->beacon_data);

    if(app->is_active) {
        // The extra-beacon API requires stop -> configure -> start for any
        // data change; there is no "update while running" call.
        furi_hal_bt_extra_beacon_stop();
    }
    furi_check(furi_hal_bt_extra_beacon_set_data(app->beacon_data, app->beacon_data_len));
    if(app->is_active) {
        furi_check(furi_hal_bt_extra_beacon_start());
    }
}

void ble_toolkit_app_set_active(BleToolkitApp* app, bool active) {
    if(active == app->is_active) return;

    furi_hal_bt_extra_beacon_stop();
    if(active) {
        furi_check(furi_hal_bt_extra_beacon_start());
    }
    app->is_active = active;
}

static void ble_toolkit_app_init_beacon_config(BleToolkitApp* app) {
    const GapExtraBeaconConfig* existing = furi_hal_bt_extra_beacon_get_config();
    if(existing) {
        memcpy(&app->beacon_config, existing, sizeof(app->beacon_config));
        return;
    }

    app->beacon_config.min_adv_interval_ms = 100;
    app->beacon_config.max_adv_interval_ms = 200;
    app->beacon_config.adv_channel_map = GapAdvChannelMapAll;
    app->beacon_config.adv_power_level = GapAdvPowerLevel_0dBm;
    app->beacon_config.address_type = GapAddressTypeRandom;
    furi_hal_random_fill_buf(app->beacon_config.address, sizeof(app->beacon_config.address));
    // Top two bits set = BLE static random address, required by the spec
    // for this address type.
    app->beacon_config.address[5] |= 0xC0;

    furi_check(furi_hal_bt_extra_beacon_set_config(&app->beacon_config));
}

static BleToolkitApp* ble_toolkit_app_alloc(void) {
    BleToolkitApp* app = malloc(sizeof(BleToolkitApp));

    app->gui = furi_record_open(RECORD_GUI);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);

    app->scene_manager = scene_manager_alloc(&ble_toolkit_app_scene_handlers, app);
    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(
        app->view_dispatcher, ble_toolkit_app_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(
        app->view_dispatcher, ble_toolkit_app_back_event_callback);
    view_dispatcher_set_tick_event_callback(
        app->view_dispatcher, ble_toolkit_app_tick_event_callback, BLE_TOOLKIT_TICK_PERIOD_MS);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    app->submenu = submenu_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, BleToolkitAppViewSubmenu, submenu_get_view(app->submenu));

    app->dialog_ex = dialog_ex_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, BleToolkitAppViewDialog, dialog_ex_get_view(app->dialog_ex));

    app->status_string = furi_string_alloc();

    app->mode = BleToolkitModeBeacon;
    app->is_active = furi_hal_bt_extra_beacon_is_active();
    ble_toolkit_app_init_beacon_config(app);
    app->beacon_data_len = furi_hal_bt_extra_beacon_get_data(app->beacon_data);

    return app;
}

static void ble_toolkit_app_free(BleToolkitApp* app) {
    // Never leave the radio broadcasting after the app closes.
    ble_toolkit_app_set_active(app, false);

    view_dispatcher_remove_view(app->view_dispatcher, BleToolkitAppViewSubmenu);
    view_dispatcher_remove_view(app->view_dispatcher, BleToolkitAppViewDialog);

    submenu_free(app->submenu);
    dialog_ex_free(app->dialog_ex);
    furi_string_free(app->status_string);

    scene_manager_free(app->scene_manager);
    view_dispatcher_free(app->view_dispatcher);

    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_GUI);

    free(app);
}

int32_t ble_toolkit_app(void* args) {
    UNUSED(args);

    BleToolkitApp* app = ble_toolkit_app_alloc();

    scene_manager_next_scene(app->scene_manager, BleToolkitAppSceneMainMenu);
    view_dispatcher_run(app->view_dispatcher);

    ble_toolkit_app_free(app);
    return 0;
}
