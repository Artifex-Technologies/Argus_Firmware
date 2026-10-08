#pragma once

#include <gui/scene_manager.h>

#define ADD_SCENE(prefix, name, id) BleToolkitAppScene##id,
typedef enum {
#include "ble_toolkit_scene_config.h"
    BleToolkitAppSceneNum,
} BleToolkitAppScene;
#undef ADD_SCENE

extern const SceneManagerHandlers ble_toolkit_app_scene_handlers;

#define ADD_SCENE(prefix, name, id) void prefix##_scene_##name##_on_enter(void*);
#include "ble_toolkit_scene_config.h"
#undef ADD_SCENE

#define ADD_SCENE(prefix, name, id) \
    bool prefix##_scene_##name##_on_event(void* context, SceneManagerEvent event);
#include "ble_toolkit_scene_config.h"
#undef ADD_SCENE

#define ADD_SCENE(prefix, name, id) void prefix##_scene_##name##_on_exit(void* context);
#include "ble_toolkit_scene_config.h"
#undef ADD_SCENE
