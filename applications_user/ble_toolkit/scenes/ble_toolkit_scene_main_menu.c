#include "../ble_toolkit.h"

static void ble_toolkit_app_main_menu_callback(void* context, uint32_t index) {
    BleToolkitApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void ble_toolkit_app_scene_main_menu_on_enter(void* context) {
    BleToolkitApp* app = context;
    Submenu* submenu = app->submenu;

    submenu_reset(submenu);
    for(BleToolkitMode mode = 0; mode < BleToolkitModeCount; mode++) {
        submenu_add_item(
            submenu, ble_toolkit_mode_names[mode], mode, ble_toolkit_app_main_menu_callback, app);
    }
    submenu_set_selected_item(submenu, app->mode);

    view_dispatcher_switch_to_view(app->view_dispatcher, BleToolkitAppViewSubmenu);
}

bool ble_toolkit_app_scene_main_menu_on_event(void* context, SceneManagerEvent event) {
    BleToolkitApp* app = context;

    if(event.type == SceneManagerEventTypeCustom) {
        app->mode = (BleToolkitMode)event.event;
        ble_toolkit_app_set_active(app, false);
        ble_toolkit_app_apply_payload(app);
        scene_manager_next_scene(app->scene_manager, BleToolkitAppSceneRun);
        return true;
    }

    return false;
}

void ble_toolkit_app_scene_main_menu_on_exit(void* context) {
    BleToolkitApp* app = context;
    submenu_reset(app->submenu);
}
