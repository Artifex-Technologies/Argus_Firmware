/**
 * @file rf_monitor.c
 * RF Monitor - live sub-GHz RSSI signal-strength meter.
 *
 * A first-party Argus app. Two things distinguish it from the stock
 * Sub-GHz app, and both are deliberate:
 *
 *  - It treats the external CC1101 (cc1101_ext) as a first-class radio,
 *    selectable at runtime alongside the internal one, rather than
 *    assuming the internal radio (see CLAUDE.md "External sub-GHz radio").
 *  - It produces a plain RSSI sample stream, which is exactly the kind of
 *    decoded/decimated data the Chymaera OS streaming budget can carry
 *    (see CLAUDE.md "The BLE budget"). The on-screen meter is the local
 *    view of that same stream; wiring it to DataExchange is future work.
 *
 * Radio access goes through the lib/subghz device abstraction so the same
 * code drives either radio. The device registry loads the external plugin
 * from the SD card at init; if it is absent the app runs internal-only.
 */
#include <furi.h>
#include <gui/gui.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <gui/elements.h>
#include <input/input.h>
#include <storage/storage.h>
#include <string.h>

#include <lib/subghz/devices/devices.h>
#include <lib/subghz/devices/cc1101_int/cc1101_int_interconnect.h>
#include <applications/drivers/subghz/cc1101_ext/cc1101_ext_interconnect.h>

#define TAG "RfMonitor"

// Graph occupies the lower part of the 128x64 screen.
#define RF_MON_HISTORY   112
#define RF_MON_POLL_MS   50
#define RF_MON_RSSI_MIN  (-110.0f)
#define RF_MON_RSSI_MAX  (-40.0f)
#define RF_MON_GRAPH_TOP 30
#define RF_MON_GRAPH_BOT 62

// RSSI captures land here so the App's capture.pull path can retrieve them
// (see CLAUDE.md streaming section and Argus_App SARINA_API.md).
#define RF_MON_LOG_DIR EXT_PATH("rf_monitor")

// Common ISM / LPD frequencies inside the CC1101's valid bands. Invalid
// entries for the current radio are skipped when stepping.
static const uint32_t rf_mon_freqs[] = {
    300000000, 315000000, 348000000, 390000000, 418000000, 433075000,
    433920000, 434420000, 438900000, 464000000, 779000000, 868350000,
    915000000, 925000000,
};
#define RF_MON_FREQ_COUNT (sizeof(rf_mon_freqs) / sizeof(rf_mon_freqs[0]))

typedef struct {
    uint32_t frequency;
    const char* radio_name;
    bool external_selected;
    bool external_available;
    bool running;
    bool logging;
    float rssi;
    float peak;
    int8_t history[RF_MON_HISTORY];
    size_t history_pos;
} RfMonitorModel;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    View* view;
    FuriTimer* timer;

    FuriMutex* mutex; // guards radio_on/device/log_file against timer vs input threads
    const SubGhzDevice* device_int;
    const SubGhzDevice* device_ext; // NULL if the plugin/module is absent
    const SubGhzDevice* device; // currently bound radio
    size_t freq_index;
    bool radio_on;

    Storage* storage;
    File* log_file;
    bool logging;
} RfMonitorApp;

// ---- SD capture logging (callers hold app->mutex) -----------------------

static bool rf_monitor_log_start(RfMonitorApp* app) {
    storage_common_mkdir(app->storage, RF_MON_LOG_DIR);
    FuriString* path = furi_string_alloc_printf(
        "%s/rssi_%lu.csv", RF_MON_LOG_DIR, (unsigned long)furi_get_tick());
    bool ok = storage_file_open(
        app->log_file, furi_string_get_cstr(path), FSAM_WRITE, FSOM_CREATE_ALWAYS);
    furi_string_free(path);
    if(ok) {
        const char* hdr = "tick_ms,freq_hz,rssi_dbm\n";
        storage_file_write(app->log_file, hdr, strlen(hdr));
        app->logging = true;
    } else {
        storage_file_close(app->log_file); // required even on failed open
    }
    return ok;
}

static void rf_monitor_log_stop(RfMonitorApp* app) {
    if(!app->logging) return;
    storage_file_close(app->log_file);
    app->logging = false;
}

// ---- radio control (all callers hold app->mutex) ------------------------

static const SubGhzDevice* rf_monitor_selected_device(RfMonitorApp* app, bool external) {
    if(external && app->device_ext) return app->device_ext;
    return app->device_int;
}

static void rf_monitor_radio_stop(RfMonitorApp* app) {
    if(!app->radio_on || !app->device) return;
    subghz_devices_idle(app->device);
    subghz_devices_sleep(app->device);
    subghz_devices_end(app->device);
    app->radio_on = false;
}

// Bind the given device at the current frequency and drop into RX. Returns
// false (and leaves the radio stopped) if the device will not start.
static bool rf_monitor_radio_start(RfMonitorApp* app, const SubGhzDevice* device) {
    rf_monitor_radio_stop(app);
    app->device = device;

    if(!subghz_devices_begin(device)) {
        subghz_devices_end(device);
        return false;
    }
    subghz_devices_reset(device);
    subghz_devices_load_preset(device, FuriHalSubGhzPresetOok650Async, NULL);
    subghz_devices_set_frequency(device, rf_mon_freqs[app->freq_index]);
    subghz_devices_flush_rx(device);
    subghz_devices_set_rx(device);
    app->radio_on = true;
    return true;
}

// Re-tune without a full stop/start; cheaper and avoids re-acquiring SPI.
static void rf_monitor_radio_retune(RfMonitorApp* app) {
    if(!app->radio_on || !app->device) return;
    subghz_devices_idle(app->device);
    subghz_devices_set_frequency(app->device, rf_mon_freqs[app->freq_index]);
    subghz_devices_flush_rx(app->device);
    subghz_devices_set_rx(app->device);
}

// ---- model sync helpers -------------------------------------------------

static void rf_monitor_reset_history(RfMonitorModel* m) {
    for(size_t i = 0; i < RF_MON_HISTORY; i++) m->history[i] = (int8_t)RF_MON_RSSI_MIN;
    m->history_pos = 0;
    m->peak = RF_MON_RSSI_MIN;
    m->rssi = RF_MON_RSSI_MIN;
}

// ---- timer: sample RSSI -------------------------------------------------

static void rf_monitor_timer_cb(void* context) {
    RfMonitorApp* app = context;

    furi_mutex_acquire(app->mutex, FuriWaitForever);
    bool on = app->radio_on;
    float rssi = RF_MON_RSSI_MIN;
    if(on && app->device) {
        rssi = subghz_devices_get_rssi(app->device);
        if(app->logging && storage_file_is_open(app->log_file)) {
            char line[48];
            int n = snprintf(
                line,
                sizeof(line),
                "%lu,%lu,%d\n",
                (unsigned long)furi_get_tick(),
                (unsigned long)rf_mon_freqs[app->freq_index],
                (int)rssi);
            storage_file_write(app->log_file, line, n);
        }
    }
    furi_mutex_release(app->mutex);

    if(!on) return;

    with_view_model(
        app->view,
        RfMonitorModel * m,
        {
            if(m->running) {
                m->rssi = rssi;
                if(rssi > m->peak) m->peak = rssi;
                m->history[m->history_pos] = (int8_t)rssi;
                m->history_pos = (m->history_pos + 1) % RF_MON_HISTORY;
            }
        },
        true);
}

// ---- drawing ------------------------------------------------------------

static int rf_monitor_rssi_to_y(float rssi) {
    if(rssi < RF_MON_RSSI_MIN) rssi = RF_MON_RSSI_MIN;
    if(rssi > RF_MON_RSSI_MAX) rssi = RF_MON_RSSI_MAX;
    float frac = (rssi - RF_MON_RSSI_MIN) / (RF_MON_RSSI_MAX - RF_MON_RSSI_MIN);
    int h = (int)(frac * (RF_MON_GRAPH_BOT - RF_MON_GRAPH_TOP));
    return RF_MON_GRAPH_BOT - h;
}

static void rf_monitor_draw(Canvas* canvas, void* model) {
    RfMonitorModel* m = model;
    canvas_clear(canvas);
    char buf[32];

    // Header: frequency and active radio.
    canvas_set_font(canvas, FontSecondary);
    snprintf(
        buf,
        sizeof(buf),
        "%lu.%03lu MHz",
        (unsigned long)(m->frequency / 1000000UL),
        (unsigned long)((m->frequency % 1000000UL) / 1000UL));
    canvas_draw_str(canvas, 2, 9, buf);
    canvas_draw_str_aligned(canvas, 126, 9, AlignRight, AlignBottom, m->radio_name);

    // Current RSSI, peak, and run/pause state.
    canvas_set_font(canvas, FontPrimary);
    snprintf(buf, sizeof(buf), "%d dBm", (int)m->rssi);
    canvas_draw_str(canvas, 2, 24, buf);

    canvas_set_font(canvas, FontSecondary);
    snprintf(buf, sizeof(buf), "pk %d", (int)m->peak);
    canvas_draw_str(canvas, 62, 24, buf);
    const char* state = m->running ? (m->logging ? "REC" : "RUN") : (m->logging ? "LOG" : "PAUSE");
    canvas_draw_str_aligned(canvas, 126, 24, AlignRight, AlignBottom, state);

    // Graph side rails.
    canvas_draw_line(canvas, 0, RF_MON_GRAPH_TOP, 0, RF_MON_GRAPH_BOT);
    canvas_draw_line(
        canvas, RF_MON_HISTORY + 1, RF_MON_GRAPH_TOP, RF_MON_HISTORY + 1, RF_MON_GRAPH_BOT);

    // History, oldest sample on the left.
    for(size_t i = 0; i < RF_MON_HISTORY; i++) {
        size_t idx = (m->history_pos + i) % RF_MON_HISTORY;
        int y = rf_monitor_rssi_to_y((float)m->history[idx]);
        canvas_draw_line(canvas, i + 1, RF_MON_GRAPH_BOT, i + 1, y);
    }
}

// ---- input --------------------------------------------------------------

static bool rf_monitor_input(InputEvent* event, void* context) {
    RfMonitorApp* app = context;

    // Long-press OK toggles SD capture logging.
    if(event->type == InputTypeLong && event->key == InputKeyOk) {
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        bool now;
        if(app->logging) {
            rf_monitor_log_stop(app);
            now = false;
        } else {
            now = rf_monitor_log_start(app);
        }
        furi_mutex_release(app->mutex);
        with_view_model(app->view, RfMonitorModel * m, { m->logging = now; }, true);
        return true;
    }

    if(event->type != InputTypeShort && event->type != InputTypeRepeat) return false;

    bool handled = true;
    switch(event->key) {
    case InputKeyLeft:
    case InputKeyRight: {
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        // Step to the next valid frequency for the bound radio.
        const SubGhzDevice* dev = app->device ? app->device : app->device_int;
        size_t start = app->freq_index;
        do {
            if(event->key == InputKeyRight) {
                app->freq_index = (app->freq_index + 1) % RF_MON_FREQ_COUNT;
            } else {
                app->freq_index =
                    (app->freq_index + RF_MON_FREQ_COUNT - 1) % RF_MON_FREQ_COUNT;
            }
        } while(!subghz_devices_is_frequency_valid(dev, rf_mon_freqs[app->freq_index]) &&
                app->freq_index != start);
        uint32_t freq = rf_mon_freqs[app->freq_index];
        rf_monitor_radio_retune(app);
        furi_mutex_release(app->mutex);

        with_view_model(
            app->view,
            RfMonitorModel * m,
            {
                m->frequency = freq;
                rf_monitor_reset_history(m);
            },
            true);
        break;
    }
    case InputKeyUp:
    case InputKeyDown: {
        // Toggle internal/external, only if the external radio is present.
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        bool want_ext = !(app->device == app->device_ext);
        if(want_ext && !app->device_ext) {
            furi_mutex_release(app->mutex);
            break;
        }
        const SubGhzDevice* dev = rf_monitor_selected_device(app, want_ext);
        bool ok = true;
        if(app->radio_on) {
            ok = rf_monitor_radio_start(app, dev);
        } else {
            app->device = dev;
        }
        const char* name = subghz_devices_get_name(app->device);
        furi_mutex_release(app->mutex);

        with_view_model(
            app->view,
            RfMonitorModel * m,
            {
                m->external_selected = want_ext && ok;
                m->radio_name = name ? name : "int";
                rf_monitor_reset_history(m);
            },
            true);
        break;
    }
    case InputKeyOk: {
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        bool now_running;
        if(app->radio_on) {
            rf_monitor_radio_stop(app);
            now_running = false;
        } else {
            const SubGhzDevice* dev = app->device ? app->device : app->device_int;
            now_running = rf_monitor_radio_start(app, dev);
        }
        const char* name = subghz_devices_get_name(app->device);
        furi_mutex_release(app->mutex);

        with_view_model(
            app->view,
            RfMonitorModel * m,
            {
                m->running = now_running;
                m->radio_name = name ? name : "int";
                if(now_running) rf_monitor_reset_history(m);
            },
            true);
        break;
    }
    default:
        handled = false;
        break;
    }
    return handled;
}

static uint32_t rf_monitor_exit(void* context) {
    UNUSED(context);
    return VIEW_NONE;
}

// ---- app lifecycle ------------------------------------------------------

static RfMonitorApp* rf_monitor_app_alloc(void) {
    RfMonitorApp* app = malloc(sizeof(RfMonitorApp));

    subghz_devices_init();
    app->device_int = subghz_devices_get_by_name(SUBGHZ_DEVICE_CC1101_INT_NAME);
    app->device_ext = subghz_devices_get_by_name(SUBGHZ_DEVICE_CC1101_EXT_NAME);
    app->device = app->device_int;
    app->freq_index = 6; // 433.92 MHz
    app->radio_on = false;
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);

    app->storage = furi_record_open(RECORD_STORAGE);
    app->log_file = storage_file_alloc(app->storage);
    app->logging = false;

    app->gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();

    app->view = view_alloc();
    view_set_context(app->view, app);
    view_set_draw_callback(app->view, rf_monitor_draw);
    view_set_input_callback(app->view, rf_monitor_input);
    view_set_previous_callback(app->view, rf_monitor_exit);
    view_allocate_model(app->view, ViewModelTypeLocking, sizeof(RfMonitorModel));

    with_view_model(
        app->view,
        RfMonitorModel * m,
        {
            m->frequency = rf_mon_freqs[app->freq_index];
            m->radio_name = subghz_devices_get_name(app->device_int);
            m->external_available = app->device_ext != NULL;
            m->external_selected = false;
            m->running = false;
            rf_monitor_reset_history(m);
        },
        true);

    view_dispatcher_add_view(app->view_dispatcher, 0, app->view);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_switch_to_view(app->view_dispatcher, 0);

    app->timer = furi_timer_alloc(rf_monitor_timer_cb, FuriTimerTypePeriodic, app);
    furi_timer_start(app->timer, furi_ms_to_ticks(RF_MON_POLL_MS));

    return app;
}

static void rf_monitor_app_free(RfMonitorApp* app) {
    furi_timer_stop(app->timer);
    furi_timer_free(app->timer);

    furi_mutex_acquire(app->mutex, FuriWaitForever);
    rf_monitor_log_stop(app);
    rf_monitor_radio_stop(app);
    furi_mutex_release(app->mutex);

    view_dispatcher_remove_view(app->view_dispatcher, 0);
    view_free(app->view);
    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_GUI);

    storage_file_free(app->log_file);
    furi_record_close(RECORD_STORAGE);

    subghz_devices_deinit();
    furi_mutex_free(app->mutex);
    free(app);
}

int32_t rf_monitor_app(void* args) {
    UNUSED(args);
    RfMonitorApp* app = rf_monitor_app_alloc();
    view_dispatcher_run(app->view_dispatcher);
    rf_monitor_app_free(app);
    return 0;
}
