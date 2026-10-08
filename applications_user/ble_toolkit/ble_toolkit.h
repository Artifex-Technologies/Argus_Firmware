/**
 * @file ble_toolkit.h
 * BLE Toolkit - Bluetooth LE advertising beacon and awareness-testing app.
 *
 * Everything here goes through the platform's extra-beacon API
 * (furi_hal_bt_extra_beacon_*); this app never touches BLE scanning
 * because the firmware's BLE stack has no observer/central role - see
 * CLAUDE.md "Hardware reality" for why.
 */
#pragma once

#include <furi.h>
#include <gui/gui.h>
#include <gui/scene_manager.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/dialog_ex.h>

#include <furi_hal_bt.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>

#include "ble_toolkit_payload.h"
#include "scenes/ble_toolkit_scene.h"

typedef struct {
    Gui* gui;
    SceneManager* scene_manager;
    ViewDispatcher* view_dispatcher;

    Submenu* submenu;
    DialogEx* dialog_ex;
    NotificationApp* notifications;

    FuriString* status_string;

    BleToolkitMode mode;
    GapExtraBeaconConfig beacon_config;
    uint8_t beacon_data[EXTRA_BEACON_MAX_DATA_SIZE];
    uint8_t beacon_data_len;
    bool is_active;
} BleToolkitApp;

typedef enum {
    BleToolkitAppViewSubmenu,
    BleToolkitAppViewDialog,
} BleToolkitAppView;

/* Sound/LED feedback sequences (defined in ble_toolkit.c). The AirTag chirp
 * is deliberately a standalone sequence so the deferred AirTag *detection*
 * feature can reuse it as its alert once scanning hardware exists. */
extern const NotificationSequence ble_toolkit_seq_start;
extern const NotificationSequence ble_toolkit_seq_stop;
extern const NotificationSequence ble_toolkit_seq_airtag_chirp;

/** (Re)build the payload for the app's current mode and, if active, push it
 * to the radio. Safe to call whether or not broadcasting is currently on. */
void ble_toolkit_app_apply_payload(BleToolkitApp* app);

/** Toggle broadcasting on/off for the app's current mode. */
void ble_toolkit_app_set_active(BleToolkitApp* app, bool active);
