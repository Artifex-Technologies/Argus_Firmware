#include "../ble_toolkit.h"

static void update_status_text(BleToolkitApp* app) {
    DialogEx* dialog_ex = app->dialog_ex;

    dialog_ex_set_header(dialog_ex, "BLE Toolkit", 64, 0, AlignCenter, AlignTop);

    FuriString* status = app->status_string;
    furi_string_reset(status);

    furi_string_cat_printf(status, "%s\n", ble_toolkit_mode_names[app->mode]);
    furi_string_cat_str(status, app->is_active ? "Status: Broadcasting\n" : "Status: Stopped\n");

    if(ble_toolkit_mode_should_cycle(app->mode)) {
        furi_string_cat_str(status, "Payload cycles every 2s");
    } else {
        furi_string_cat_printf(status, "Payload: %d bytes", app->beacon_data_len);
    }

    dialog_ex_set_text(dialog_ex, furi_string_get_cstr(status), 0, 20, AlignLeft, AlignTop);

    dialog_ex_set_left_button_text(dialog_ex, "Modes");
    dialog_ex_set_center_button_text(dialog_ex, app->is_active ? "Stop" : "Start");
}

static void ble_toolkit_app_run_dialog_callback(DialogExResult result, void* context) {
    BleToolkitApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, result);
}

void ble_toolkit_app_scene_run_on_enter(void* context) {
    BleToolkitApp* app = context;

    dialog_ex_reset(app->dialog_ex);
    dialog_ex_set_result_callback(app->dialog_ex, ble_toolkit_app_run_dialog_callback);
    dialog_ex_set_context(app->dialog_ex, app);
    update_status_text(app);

    view_dispatcher_switch_to_view(app->view_dispatcher, BleToolkitAppViewDialog);
}

bool ble_toolkit_app_scene_run_on_event(void* context, SceneManagerEvent event) {
    BleToolkitApp* app = context;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == DialogExResultLeft) {
            ble_toolkit_app_set_active(app, false);
            scene_manager_next_scene(app->scene_manager, BleToolkitAppSceneMainMenu);
            return true;
        } else if(event.event == DialogExResultCenter) {
            ble_toolkit_app_set_active(app, !app->is_active);
            notification_message(
                app->notifications,
                app->is_active ? &ble_toolkit_seq_start : &ble_toolkit_seq_stop);
            update_status_text(app);
            return true;
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        if(app->is_active && ble_toolkit_mode_should_cycle(app->mode)) {
            ble_toolkit_app_apply_payload(app);
        }
        if(app->is_active && app->mode == BleToolkitModeAirTag) {
            notification_message(app->notifications, &ble_toolkit_seq_airtag_chirp);
        }
        return true;
    }

    return false;
}

void ble_toolkit_app_scene_run_on_exit(void* context) {
    UNUSED(context);
}
