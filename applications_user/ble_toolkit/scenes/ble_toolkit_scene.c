#include "ble_toolkit_scene.h"

#define ADD_SCENE(prefix, name, id) prefix##_scene_##name##_on_enter,
void (*const ble_toolkit_app_on_enter_handlers[])(void*) = {
#include "ble_toolkit_scene_config.h"
};
#undef ADD_SCENE

#define ADD_SCENE(prefix, name, id) prefix##_scene_##name##_on_event,
bool (*const ble_toolkit_app_on_event_handlers[])(void* context, SceneManagerEvent event) = {
#include "ble_toolkit_scene_config.h"
};
#undef ADD_SCENE

#define ADD_SCENE(prefix, name, id) prefix##_scene_##name##_on_exit,
void (*const ble_toolkit_app_on_exit_handlers[])(void* context) = {
#include "ble_toolkit_scene_config.h"
};
#undef ADD_SCENE

const SceneManagerHandlers ble_toolkit_app_scene_handlers = {
    .on_enter_handlers = ble_toolkit_app_on_enter_handlers,
    .on_event_handlers = ble_toolkit_app_on_event_handlers,
    .on_exit_handlers = ble_toolkit_app_on_exit_handlers,
    .scene_num = BleToolkitAppSceneNum,
};
