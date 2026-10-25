/**
 * @file ld2450_radar.c
 * Radar - live display for an HLK-LD2450 24 GHz mmWave radar on GPIO.
 *
 * A first-party Argus app. The LD2450 tracks up to three people with X/Y
 * position and speed, out to ~6 m across a ~120 degree cone, and reports
 * over UART. That gives a real "where is someone" view, which no radio
 * inside the Flipper can (see CLAUDE.md "Hardware reality"). The sensor
 * does the tracking; this app is the host side of the link, the same split
 * as every other external module.
 *
 * Wiring (sensor -> Flipper): 5V -> pin 1, GND -> pin 8, TX -> RX, RX -> TX,
 * on either USART (pins 13 TX / 14 RX) or LPUART (15 TX / 16 RX), selected
 * in Settings. LPUART leaves 13/14 free for the Wi-Fi devboard.
 *
 * Demo mode feeds synthetic frames through the same parser, so the whole
 * path except the wire can be exercised without a sensor.
 *
 * Protocol: ld2450.h. Tracking data can be logged to SD for the App's
 * capture.pull path.
 */
#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <gui/elements.h>
#include <gui/modules/variable_item_list.h>
#include <expansion/expansion.h>
#include <flipper_format/flipper_format.h>
#include <input/input.h>
#include <storage/storage.h>
#include <math.h>
#include <string.h>

#include "ld2450.h"

#define TAG "Ld2450Radar"

#define RADAR_RX_STREAM      512
#define RADAR_TIMEOUT_MS     1000 // no frames for this long = no sensor
#define RADAR_ACK_TIMEOUT_MS 500
#define RADAR_TRAIL          10
#define RADAR_DEMO_PERIOD_MS 100

#define RADAR_LOG_DIR       EXT_PATH("ld2450")
#define RADAR_SETTINGS_DIR  EXT_PATH("apps_data/ld2450_radar")
#define RADAR_SETTINGS_PATH RADAR_SETTINGS_DIR "/settings.txt"
#define RADAR_SETTINGS_TYPE "Argus LD2450 Radar Settings"

// Screen geometry: the sensor sits at the bottom centre, the cone opens up.
#define RADAR_CX       64
#define RADAR_CY       63
#define RADAR_R_PX     60
#define RADAR_HALF_FOV 60 // degrees either side of boresight
#define RADAR_ARC_PTS  31

typedef enum {
    RadarViewMain,
    RadarViewSettings,
} RadarView;

typedef enum {
    RadarPortUsart, // pins 13/14
    RadarPortLpuart, // pins 15/16
    RadarPortCount,
} RadarPort;

typedef enum {
    RadarCmdSetPort,
    RadarCmdDemo,
    RadarCmdTracking, // arg: 0 multi, 1 single
    RadarCmdBluetooth, // arg: 0 off, 1 on
    RadarCmdVersion,
} RadarCmdType;

typedef struct {
    RadarCmdType type;
    uint32_t arg;
} RadarCmd;

typedef enum {
    RadarEventVersion,
} RadarEvent;

typedef enum {
    RadarSettingPort,
    RadarSettingMirror,
    RadarSettingDemo,
    RadarSettingTracking,
    RadarSettingBluetooth,
    RadarSettingVersion,
} RadarSetting;

typedef struct {
    bool connected;
    bool demo;
    Ld2450Target target[LD2450_MAX_TARGETS];
    int16_t trail_x[LD2450_MAX_TARGETS][RADAR_TRAIL];
    int16_t trail_y[LD2450_MAX_TARGETS][RADAR_TRAIL];
    uint8_t trail_pos[LD2450_MAX_TARGETS];
    uint8_t trail_len[LD2450_MAX_TARGETS];
    uint16_t range_mm;
    bool list_view;
    bool mirror;
    bool logging;
    RadarPort port;
    const char* toast;
    uint32_t toast_until;
} RadarModel;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    View* main_view;
    VariableItemList* settings;
    VariableItem* version_item;

    Expansion* expansion;
    FuriHalSerialHandle* serial;
    FuriStreamBuffer* rx_stream;
    FuriMessageQueue* cmd_queue;
    FuriThread* worker;
    volatile bool worker_run;
    bool otg_enabled_by_us;

    Ld2450Parser parser; // worker thread only
    RadarPort port;
    bool demo;
    bool mirror;
    char version[24];

    FuriMutex* log_mutex; // guards log_file/logging: worker vs input thread
    Storage* storage;
    File* log_file;
    bool logging;
} RadarApp;

static const char* const radar_port_names[RadarPortCount] = {"13/14", "15/16"};
static const char* const radar_on_off[] = {"Off", "On"};
static const char* const radar_tracking_names[] = {"Multi", "Single"};

static float radar_arc_sin[RADAR_ARC_PTS];
static float radar_arc_cos[RADAR_ARC_PTS];

// ---- settings persistence --------------------------------------------------

static void radar_settings_load(RadarApp* app) {
    FlipperFormat* ff = flipper_format_file_alloc(app->storage);
    FuriString* type = furi_string_alloc();
    uint32_t version = 0, port = 0;
    bool mirror = false;
    if(flipper_format_file_open_existing(ff, RADAR_SETTINGS_PATH) &&
       flipper_format_read_header(ff, type, &version) &&
       furi_string_equal(type, RADAR_SETTINGS_TYPE) && version == 1) {
        if(flipper_format_read_uint32(ff, "Port", &port, 1) && port < RadarPortCount) {
            app->port = port;
        }
        if(flipper_format_read_bool(ff, "Mirror", &mirror, 1)) app->mirror = mirror;
    }
    furi_string_free(type);
    flipper_format_free(ff);
}

static void radar_settings_save(RadarApp* app) {
    storage_simply_mkdir(app->storage, RADAR_SETTINGS_DIR);
    FlipperFormat* ff = flipper_format_file_alloc(app->storage);
    uint32_t port = app->port;
    if(flipper_format_file_open_always(ff, RADAR_SETTINGS_PATH)) {
        flipper_format_write_header_cstr(ff, RADAR_SETTINGS_TYPE, 1);
        flipper_format_write_uint32(ff, "Port", &port, 1);
        flipper_format_write_bool(ff, "Mirror", &app->mirror, 1);
    }
    flipper_format_free(ff);
}

// ---- UART (worker thread) ---------------------------------------------------

static void
    radar_serial_rx_cb(FuriHalSerialHandle* handle, FuriHalSerialRxEvent event, void* ctx) {
    RadarApp* app = ctx;
    if(event & FuriHalSerialRxEventData) {
        while(furi_hal_serial_async_rx_available(handle)) {
            uint8_t c = furi_hal_serial_async_rx(handle);
            furi_stream_buffer_send(app->rx_stream, &c, 1, 0);
        }
    }
}

static void radar_serial_open(RadarApp* app, RadarPort port) {
    FuriHalSerialId id = port == RadarPortUsart ? FuriHalSerialIdUsart : FuriHalSerialIdLpuart;
    app->serial = furi_hal_serial_control_acquire(id);
    if(!app->serial) {
        FURI_LOG_E(TAG, "UART busy");
        return;
    }
    furi_hal_serial_init(app->serial, LD2450_DEFAULT_BAUD);
    furi_hal_serial_async_rx_start(app->serial, radar_serial_rx_cb, app, false);
}

static void radar_serial_close(RadarApp* app) {
    if(!app->serial) return;
    furi_hal_serial_async_rx_stop(app->serial);
    furi_hal_serial_deinit(app->serial);
    furi_hal_serial_control_release(app->serial);
    app->serial = NULL;
    furi_stream_buffer_reset(app->rx_stream);
    ld2450_parser_reset(&app->parser);
}

// ---- SD logging (callers hold log_mutex) -----------------------------------

static bool radar_log_start(RadarApp* app) {
    storage_common_mkdir(app->storage, RADAR_LOG_DIR);
    FuriString* path = furi_string_alloc_printf(
        "%s/radar_%lu.csv", RADAR_LOG_DIR, (unsigned long)furi_get_tick());
    bool ok = storage_file_open(
        app->log_file, furi_string_get_cstr(path), FSAM_WRITE, FSOM_CREATE_ALWAYS);
    furi_string_free(path);
    if(ok) {
        const char* hdr = "tick_ms,target,x_mm,y_mm,speed_cm_s,resolution_mm\n";
        storage_file_write(app->log_file, hdr, strlen(hdr));
        app->logging = true;
    } else {
        storage_file_close(app->log_file); // required even on failed open
    }
    return ok;
}

static void radar_log_stop(RadarApp* app) {
    if(!app->logging) return;
    storage_file_close(app->log_file);
    app->logging = false;
}

static void radar_log_frame(RadarApp* app, const Ld2450Frame* frame) {
    furi_mutex_acquire(app->log_mutex, FuriWaitForever);
    if(app->logging) {
        uint32_t now = furi_get_tick();
        for(size_t i = 0; i < LD2450_MAX_TARGETS; i++) {
            const Ld2450Target* t = &frame->target[i];
            if(!t->present) continue;
            char line[64];
            int n = snprintf(
                line,
                sizeof(line),
                "%lu,%u,%d,%d,%d,%u\n",
                (unsigned long)now,
                (unsigned)(i + 1),
                t->x_mm,
                t->y_mm,
                t->speed_cm_s,
                t->resolution_mm);
            if(n > 0) storage_file_write(app->log_file, line, n);
        }
    }
    furi_mutex_release(app->log_mutex);
}

// ---- frames into the model --------------------------------------------------

static void radar_toast(RadarApp* app, const char* text) {
    with_view_model(
        app->main_view,
        RadarModel * m,
        {
            m->toast = text;
            m->toast_until = furi_get_tick() + furi_ms_to_ticks(1500);
        },
        true);
}

static void radar_apply_frame(RadarApp* app, const Ld2450Frame* frame) {
    with_view_model(
        app->main_view,
        RadarModel * m,
        {
            m->connected = true;
            for(size_t i = 0; i < LD2450_MAX_TARGETS; i++) {
                const Ld2450Target* t = &frame->target[i];
                m->target[i] = *t;
                if(t->present) {
                    m->trail_x[i][m->trail_pos[i]] = t->x_mm;
                    m->trail_y[i][m->trail_pos[i]] = t->y_mm;
                    m->trail_pos[i] = (m->trail_pos[i] + 1) % RADAR_TRAIL;
                    if(m->trail_len[i] < RADAR_TRAIL) m->trail_len[i]++;
                } else {
                    m->trail_len[i] = 0;
                }
            }
        },
        true);
    radar_log_frame(app, frame);
}

// Feed received bytes into the parser. Data frames go to the display; if
// want_ack is non-zero, a matching ACK's result is written to *acked/*ok.
static bool
    radar_pump(RadarApp* app, uint32_t timeout_ms, uint16_t want_ack, bool* acked, bool* ok) {
    uint8_t buf[64];
    bool got_frame = false;
    size_t n = furi_stream_buffer_receive(app->rx_stream, buf, sizeof(buf), timeout_ms);
    for(size_t i = 0; i < n; i++) {
        Ld2450Event e = ld2450_parser_feed(&app->parser, buf[i]);
        if(e == Ld2450EventFrame && !app->demo) {
            radar_apply_frame(app, &app->parser.frame);
            got_frame = true;
        } else if(
            e == Ld2450EventAck && want_ack && !*acked &&
            (app->parser.ack.command & 0xFF) == (want_ack & 0xFF)) {
            *acked = true;
            *ok = ld2450_ack_ok(&app->parser.ack, want_ack);
            if(*ok && want_ack == LD2450_CMD_QUERY_VERSION) {
                ld2450_ack_version(&app->parser.ack, app->version, sizeof(app->version));
            }
        }
    }
    return got_frame;
}

// Send one command and wait for its ACK.
static bool radar_command(RadarApp* app, uint16_t command, const uint8_t* value, size_t len) {
    if(!app->serial) return false;
    uint8_t frame[32];
    size_t n = ld2450_build_command(frame, sizeof(frame), command, value, len);
    furi_hal_serial_tx(app->serial, frame, n);
    furi_hal_serial_tx_wait_complete(app->serial);

    bool acked = false, ok = false;
    uint32_t start = furi_get_tick();
    while(!acked && furi_get_tick() - start < furi_ms_to_ticks(RADAR_ACK_TIMEOUT_MS)) {
        radar_pump(app, 20, command, &acked, &ok);
    }
    if(!acked) FURI_LOG_W(TAG, "no ACK for 0x%04X", command);
    return ok;
}

// Commands only take effect between enable-config and end-config.
static bool radar_configure(RadarApp* app, uint16_t command, const uint8_t* value, size_t len) {
    const uint8_t enable[] = {0x01, 0x00};
    if(!radar_command(app, LD2450_CMD_ENABLE_CONFIG, enable, sizeof(enable))) return false;
    bool ok = radar_command(app, command, value, len);
    radar_command(app, LD2450_CMD_END_CONFIG, NULL, 0);
    return ok;
}

// ---- demo -------------------------------------------------------------------

// Three made-up people: one pacing across the room, one walking toward the
// sensor and back, one who comes and goes. Encoded as real frames and fed
// through the parser, so demo mode exercises the same decode path.
static void radar_demo_step(RadarApp* app, uint32_t t_ms) {
    float t = (float)t_ms / 1000.0f;
    Ld2450Frame f = {0};

    f.target[0].present = true;
    f.target[0].x_mm = (int16_t)(1800.0f * sinf(t * 0.4f));
    f.target[0].y_mm = (int16_t)(3000.0f + 400.0f * cosf(t * 0.8f));
    f.target[0].speed_cm_s = (int16_t)(-0.1f * 400.0f * 0.8f * sinf(t * 0.8f));

    float y1 = 3200.0f + 2200.0f * sinf(t * 0.25f);
    f.target[1].present = true;
    f.target[1].x_mm = -1200;
    f.target[1].y_mm = (int16_t)y1;
    f.target[1].speed_cm_s = (int16_t)(0.1f * 2200.0f * 0.25f * cosf(t * 0.25f));

    if(fmodf(t, 20.0f) < 12.0f) {
        f.target[2].present = true;
        f.target[2].x_mm = (int16_t)(600.0f + 300.0f * sinf(t));
        f.target[2].y_mm = 1400;
    }
    for(size_t i = 0; i < LD2450_MAX_TARGETS; i++) {
        if(f.target[i].present) f.target[i].resolution_mm = 360;
    }

    uint8_t bytes[LD2450_FRAME_SIZE];
    ld2450_encode_frame(bytes, &f);
    Ld2450Parser demo_parser;
    ld2450_parser_reset(&demo_parser);
    for(size_t i = 0; i < sizeof(bytes); i++) {
        if(ld2450_parser_feed(&demo_parser, bytes[i]) == Ld2450EventFrame) {
            radar_apply_frame(app, &demo_parser.frame);
        }
    }
}

// ---- worker -----------------------------------------------------------------

static void radar_handle_cmd(RadarApp* app, const RadarCmd* cmd) {
    switch(cmd->type) {
    case RadarCmdSetPort:
        radar_serial_close(app);
        radar_serial_open(app, cmd->arg);
        break;
    case RadarCmdDemo:
        app->demo = cmd->arg;
        with_view_model(app->main_view, RadarModel * m, { m->demo = app->demo; }, true);
        break;
    case RadarCmdTracking: {
        uint16_t c = cmd->arg ? LD2450_CMD_SINGLE_TARGET : LD2450_CMD_MULTI_TARGET;
        radar_toast(app, radar_configure(app, c, NULL, 0) ? "Tracking set" : "No reply");
        break;
    }
    case RadarCmdBluetooth: {
        // The sensor ships with its own BLE on, open to anyone nearby with
        // the vendor app. The change applies after a restart.
        const uint8_t value[] = {cmd->arg ? 0x01 : 0x00, 0x00};
        bool ok = radar_configure(app, LD2450_CMD_BLUETOOTH, value, sizeof(value));
        if(ok) ok = radar_configure(app, LD2450_CMD_RESTART, NULL, 0);
        radar_toast(app, ok ? (cmd->arg ? "Sensor BT on" : "Sensor BT off") : "No reply");
        break;
    }
    case RadarCmdVersion:
        if(!radar_configure(app, LD2450_CMD_QUERY_VERSION, NULL, 0)) {
            strlcpy(app->version, "No reply", sizeof(app->version));
        }
        // Widgets belong to the GUI thread; hand the result over.
        view_dispatcher_send_custom_event(app->view_dispatcher, RadarEventVersion);
        break;
    }
}

static int32_t radar_worker(void* ctx) {
    RadarApp* app = ctx;
    uint32_t last_frame = furi_get_tick();
    uint32_t last_demo = 0;

    radar_serial_open(app, app->port); // read before any SetPort can be queued

    while(app->worker_run) {
        RadarCmd cmd;
        while(furi_message_queue_get(app->cmd_queue, &cmd, 0) == FuriStatusOk) {
            radar_handle_cmd(app, &cmd);
        }

        uint32_t now = furi_get_tick();
        if(app->demo) {
            furi_stream_buffer_reset(app->rx_stream);
            if(now - last_demo >= furi_ms_to_ticks(RADAR_DEMO_PERIOD_MS)) {
                radar_demo_step(app, now);
                last_demo = now;
                last_frame = now;
            }
            furi_delay_ms(10);
        } else if(radar_pump(app, 20, 0, NULL, NULL)) {
            last_frame = now;
        }

        bool silent = now - last_frame > furi_ms_to_ticks(RADAR_TIMEOUT_MS);
        bool changed = false;
        with_view_model(
            app->main_view,
            RadarModel * m,
            {
                if(silent && m->connected) {
                    m->connected = false;
                    memset(m->target, 0, sizeof(m->target));
                    memset(m->trail_len, 0, sizeof(m->trail_len));
                    changed = true;
                }
                if(m->toast && (int32_t)(now - m->toast_until) >= 0) {
                    m->toast = NULL;
                    changed = true;
                }
            },
            changed);
    }

    radar_serial_close(app);
    return 0;
}

static void radar_send_cmd(RadarApp* app, RadarCmdType type, uint32_t arg) {
    RadarCmd cmd = {type, arg};
    furi_message_queue_put(app->cmd_queue, &cmd, FuriWaitForever);
}

// ---- drawing ----------------------------------------------------------------

static void radar_to_screen(const RadarModel* m, int16_t x_mm, int16_t y_mm, int* sx, int* sy) {
    int x = m->mirror ? -x_mm : x_mm;
    *sx = RADAR_CX + x * RADAR_R_PX / m->range_mm;
    *sy = RADAR_CY - y_mm * RADAR_R_PX / m->range_mm;
}

static void radar_draw_cone(Canvas* canvas, const RadarModel* m) {
    // Range rings at thirds of the range, dotted.
    for(int ring = 1; ring <= 3; ring++) {
        float r = (float)(RADAR_R_PX * ring) / 3.0f;
        for(int i = 0; i < RADAR_ARC_PTS; i++) {
            canvas_draw_dot(
                canvas,
                RADAR_CX + (int)(r * radar_arc_sin[i]),
                RADAR_CY - (int)(r * radar_arc_cos[i]));
        }
    }
    // Cone edges.
    canvas_draw_line(
        canvas,
        RADAR_CX,
        RADAR_CY,
        RADAR_CX + (int)(RADAR_R_PX * radar_arc_sin[0]),
        RADAR_CY - (int)(RADAR_R_PX * radar_arc_cos[0]));
    canvas_draw_line(
        canvas,
        RADAR_CX,
        RADAR_CY,
        RADAR_CX + (int)(RADAR_R_PX * radar_arc_sin[RADAR_ARC_PTS - 1]),
        RADAR_CY - (int)(RADAR_R_PX * radar_arc_cos[RADAR_ARC_PTS - 1]));

    // Ring labels down the right edge of the cone.
    char buf[8];
    canvas_set_font(canvas, FontSecondary);
    for(int ring = 1; ring <= 3; ring++) {
        int mm = m->range_mm * ring / 3;
        snprintf(buf, sizeof(buf), "%d.%dm", mm / 1000, (mm % 1000) / 100);
        int r = RADAR_R_PX * ring / 3;
        canvas_draw_str(canvas, RADAR_CX + (int)(r * 0.866f) + 3, RADAR_CY - r / 2 + 3, buf);
    }
}

static void radar_draw_targets(Canvas* canvas, const RadarModel* m) {
    char label[2] = {0};
    for(size_t i = 0; i < LD2450_MAX_TARGETS; i++) {
        // Trail, oldest first; positions outside the range are clipped.
        for(uint8_t k = 0; k < m->trail_len[i]; k++) {
            uint8_t idx = (m->trail_pos[i] + RADAR_TRAIL - m->trail_len[i] + k) % RADAR_TRAIL;
            int sx, sy;
            radar_to_screen(m, m->trail_x[i][idx], m->trail_y[i][idx], &sx, &sy);
            if(sy >= 0 && sx >= 0 && sx < 128) canvas_draw_dot(canvas, sx, sy);
        }
        const Ld2450Target* t = &m->target[i];
        if(!t->present) continue;
        int sx, sy;
        radar_to_screen(m, t->x_mm, t->y_mm, &sx, &sy);
        if(sy < 2) continue; // beyond the zoomed range
        canvas_draw_disc(canvas, sx, sy, 2);
        label[0] = (char)('1' + i);
        canvas_draw_str(canvas, sx + 4, sy + 3, label);
    }
}

// "1.23" from millimetres, with sign when asked.
static void radar_fmt_m(char* out, size_t size, int mm, bool sign) {
    const char* s = mm < 0 ? "-" : (sign ? "+" : "");
    int a = mm < 0 ? -mm : mm;
    snprintf(out, size, "%s%d.%02d", s, a / 1000, (a % 1000) / 10);
}

static void radar_draw_list(Canvas* canvas, const RadarModel* m) {
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 0, 18, "#   x m    y m   dist   cm/s");
    canvas_draw_line(canvas, 0, 20, 127, 20);
    for(size_t i = 0; i < LD2450_MAX_TARGETS; i++) {
        const Ld2450Target* t = &m->target[i];
        int y = 31 + (int)i * 11;
        char buf[64];
        if(!t->present) {
            snprintf(buf, sizeof(buf), "%u   --", (unsigned)(i + 1));
        } else {
            char xs[12], ys[12], ds[12];
            int x = m->mirror ? -t->x_mm : t->x_mm;
            int dist = (int)sqrtf((float)t->x_mm * t->x_mm + (float)t->y_mm * t->y_mm);
            radar_fmt_m(xs, sizeof(xs), x, true);
            radar_fmt_m(ys, sizeof(ys), t->y_mm, false);
            radar_fmt_m(ds, sizeof(ds), dist, false);
            snprintf(
                buf,
                sizeof(buf),
                "%u  %s  %s  %s  %+d",
                (unsigned)(i + 1),
                xs,
                ys,
                ds,
                t->speed_cm_s);
        }
        canvas_draw_str(canvas, 0, y, buf);
    }
}

static void radar_draw(Canvas* canvas, void* model) {
    RadarModel* m = model;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontSecondary);

    if(!m->connected) {
        char buf[32];
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 14, AlignCenter, AlignCenter, "No LD2450");
        canvas_set_font(canvas, FontSecondary);
        snprintf(buf, sizeof(buf), "Listening on pins %s", radar_port_names[m->port]);
        canvas_draw_str_aligned(canvas, 64, 28, AlignCenter, AlignCenter, buf);
        canvas_draw_str_aligned(canvas, 64, 38, AlignCenter, AlignCenter, "5V pin 1, GND pin 8");
        canvas_draw_str_aligned(canvas, 64, 52, AlignCenter, AlignCenter, "OK: settings / demo");
        return;
    }

    int count = 0;
    for(size_t i = 0; i < LD2450_MAX_TARGETS; i++)
        count += m->target[i].present;

    if(m->list_view) {
        radar_draw_list(canvas, m);
    } else {
        radar_draw_cone(canvas, m);
        radar_draw_targets(canvas, m);
    }

    // Header corners: target count left, mode flags right.
    char buf[24];
    snprintf(buf, sizeof(buf), "%d target%s", count, count == 1 ? "" : "s");
    canvas_draw_str(canvas, 0, 7, buf);
    const char* flag = m->toast ? m->toast : (m->demo ? "DEMO" : NULL);
    if(flag) canvas_draw_str_aligned(canvas, 127, 7, AlignRight, AlignBottom, flag);
    if(m->logging) canvas_draw_str(canvas, 0, 63, "REC");
}

// ---- input ------------------------------------------------------------------

static bool radar_input(InputEvent* event, void* ctx) {
    RadarApp* app = ctx;

    if(event->type == InputTypeLong && event->key == InputKeyOk) {
        furi_mutex_acquire(app->log_mutex, FuriWaitForever);
        bool now = app->logging ? (radar_log_stop(app), false) : radar_log_start(app);
        furi_mutex_release(app->log_mutex);
        with_view_model(app->main_view, RadarModel * m, { m->logging = now; }, true);
        return true;
    }
    if(event->type != InputTypeShort && event->type != InputTypeRepeat) return false;

    switch(event->key) {
    case InputKeyOk:
        view_dispatcher_switch_to_view(app->view_dispatcher, RadarViewSettings);
        return true;
    case InputKeyLeft:
    case InputKeyRight:
        with_view_model(app->main_view, RadarModel * m, { m->list_view = !m->list_view; }, true);
        return true;
    case InputKeyUp:
    case InputKeyDown:
        // Zoom between the full 6 m and a closer 3 m view.
        with_view_model(
            app->main_view,
            RadarModel * m,
            { m->range_mm = m->range_mm == LD2450_MAX_RANGE_MM ? 3000 : LD2450_MAX_RANGE_MM; },
            true);
        return true;
    default:
        return false;
    }
}

// ---- settings view ----------------------------------------------------------

static void radar_port_changed(VariableItem* item) {
    RadarApp* app = variable_item_get_context(item);
    uint8_t i = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, radar_port_names[i]);
    app->port = i; // GUI-side copy for saving; the worker takes it from the command
    radar_send_cmd(app, RadarCmdSetPort, i);
    with_view_model(app->main_view, RadarModel * m, { m->port = i; }, false);
    radar_settings_save(app);
}

static void radar_mirror_changed(VariableItem* item) {
    RadarApp* app = variable_item_get_context(item);
    uint8_t i = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, radar_on_off[i]);
    app->mirror = i;
    with_view_model(app->main_view, RadarModel * m, { m->mirror = i; }, false);
    radar_settings_save(app);
}

static void radar_demo_changed(VariableItem* item) {
    RadarApp* app = variable_item_get_context(item);
    uint8_t i = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, radar_on_off[i]);
    radar_send_cmd(app, RadarCmdDemo, i);
}

static void radar_tracking_changed(VariableItem* item) {
    RadarApp* app = variable_item_get_context(item);
    uint8_t i = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, radar_tracking_names[i]);
    radar_send_cmd(app, RadarCmdTracking, i);
}

static void radar_bluetooth_changed(VariableItem* item) {
    RadarApp* app = variable_item_get_context(item);
    uint8_t i = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, radar_on_off[i]);
    radar_send_cmd(app, RadarCmdBluetooth, i);
}

static void radar_settings_enter(void* ctx, uint32_t index) {
    RadarApp* app = ctx;
    if(index == RadarSettingVersion) {
        variable_item_set_current_value_text(app->version_item, "...");
        radar_send_cmd(app, RadarCmdVersion, 0);
    }
}

static bool radar_custom_event(void* ctx, uint32_t event) {
    RadarApp* app = ctx;
    if(event == RadarEventVersion) {
        variable_item_set_current_value_text(app->version_item, app->version);
        return true;
    }
    return false;
}

static uint32_t radar_exit(void* ctx) {
    UNUSED(ctx);
    return VIEW_NONE;
}

static uint32_t radar_back_to_main(void* ctx) {
    UNUSED(ctx);
    return RadarViewMain;
}

static void radar_settings_build(RadarApp* app) {
    VariableItemList* list = app->settings;
    VariableItem* item;

    item = variable_item_list_add(list, "Sensor pins", RadarPortCount, radar_port_changed, app);
    variable_item_set_current_value_index(item, app->port);
    variable_item_set_current_value_text(item, radar_port_names[app->port]);

    item = variable_item_list_add(list, "Mirror L/R", 2, radar_mirror_changed, app);
    variable_item_set_current_value_index(item, app->mirror);
    variable_item_set_current_value_text(item, radar_on_off[app->mirror]);

    item = variable_item_list_add(list, "Demo mode", 2, radar_demo_changed, app);
    variable_item_set_current_value_index(item, 0);
    variable_item_set_current_value_text(item, radar_on_off[0]);

    // These two write to the sensor. Their state cannot be read back, so they
    // show the last value sent rather than the sensor's actual setting.
    item = variable_item_list_add(list, "Tracking", 2, radar_tracking_changed, app);
    variable_item_set_current_value_index(item, 0);
    variable_item_set_current_value_text(item, radar_tracking_names[0]);

    item = variable_item_list_add(list, "Sensor BT", 2, radar_bluetooth_changed, app);
    variable_item_set_current_value_index(item, 1);
    variable_item_set_current_value_text(item, radar_on_off[1]);

    app->version_item = variable_item_list_add(list, "Firmware (OK)", 1, NULL, app);
    variable_item_set_current_value_text(app->version_item, "?");

    variable_item_list_set_enter_callback(list, radar_settings_enter, app);
}

// ---- lifecycle --------------------------------------------------------------

static RadarApp* radar_alloc(void) {
    RadarApp* app = malloc(sizeof(RadarApp));
    memset(app, 0, sizeof(*app));

    for(int i = 0; i < RADAR_ARC_PTS; i++) {
        float deg = -RADAR_HALF_FOV + (float)(2 * RADAR_HALF_FOV * i) / (RADAR_ARC_PTS - 1);
        float rad = deg * (float)M_PI / 180.0f;
        radar_arc_sin[i] = sinf(rad);
        radar_arc_cos[i] = cosf(rad);
    }

    app->storage = furi_record_open(RECORD_STORAGE);
    app->log_file = storage_file_alloc(app->storage);
    app->log_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    radar_settings_load(app);

    // The expansion service listens on the UARTs for modules; it must let go
    // before a port is acquired (see expansion.h).
    app->expansion = furi_record_open(RECORD_EXPANSION);
    expansion_disable(app->expansion);

    // The sensor runs from GPIO pin 1 (5V).
    if(!furi_hal_power_is_otg_enabled()) {
        app->otg_enabled_by_us = furi_hal_power_enable_otg();
    }

    app->rx_stream = furi_stream_buffer_alloc(RADAR_RX_STREAM, 1);
    app->cmd_queue = furi_message_queue_alloc(8, sizeof(RadarCmd));
    ld2450_parser_reset(&app->parser);

    app->gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, radar_custom_event);

    app->main_view = view_alloc();
    view_set_context(app->main_view, app);
    view_set_draw_callback(app->main_view, radar_draw);
    view_set_input_callback(app->main_view, radar_input);
    view_set_previous_callback(app->main_view, radar_exit);
    view_allocate_model(app->main_view, ViewModelTypeLocking, sizeof(RadarModel));
    with_view_model(
        app->main_view,
        RadarModel * m,
        {
            memset(m, 0, sizeof(*m));
            m->range_mm = LD2450_MAX_RANGE_MM;
            m->port = app->port;
            m->mirror = app->mirror;
        },
        false);
    view_dispatcher_add_view(app->view_dispatcher, RadarViewMain, app->main_view);

    app->settings = variable_item_list_alloc();
    radar_settings_build(app);
    view_set_previous_callback(variable_item_list_get_view(app->settings), radar_back_to_main);
    view_dispatcher_add_view(
        app->view_dispatcher, RadarViewSettings, variable_item_list_get_view(app->settings));

    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_switch_to_view(app->view_dispatcher, RadarViewMain);

    app->worker_run = true;
    app->worker = furi_thread_alloc_ex("Ld2450Worker", 2048, radar_worker, app);
    furi_thread_start(app->worker);

    return app;
}

static void radar_free(RadarApp* app) {
    app->worker_run = false;
    furi_thread_join(app->worker);
    furi_thread_free(app->worker);

    furi_message_queue_free(app->cmd_queue);
    furi_stream_buffer_free(app->rx_stream);

    if(app->otg_enabled_by_us) furi_hal_power_disable_otg();
    expansion_enable(app->expansion);
    furi_record_close(RECORD_EXPANSION);

    furi_mutex_acquire(app->log_mutex, FuriWaitForever);
    radar_log_stop(app);
    furi_mutex_release(app->log_mutex);
    storage_file_free(app->log_file);
    furi_mutex_free(app->log_mutex);
    furi_record_close(RECORD_STORAGE);

    view_dispatcher_remove_view(app->view_dispatcher, RadarViewMain);
    view_dispatcher_remove_view(app->view_dispatcher, RadarViewSettings);
    view_free(app->main_view);
    variable_item_list_free(app->settings);
    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_GUI);

    free(app);
}

int32_t ld2450_radar_app(void* args) {
    UNUSED(args);
    RadarApp* app = radar_alloc();
    view_dispatcher_run(app->view_dispatcher);
    radar_free(app);
    return 0;
}
