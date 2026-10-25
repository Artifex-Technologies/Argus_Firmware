/**
 * @file wifi_scanner.c
 * Wi-Fi Scanner - nearby networks via an ESP32 Marauder board on GPIO.
 *
 * A first-party Argus app. The Flipper has no Wi-Fi radio (see CLAUDE.md
 * "Hardware reality"), so scanning runs on the ESP32-S2 Wi-Fi devboard
 * flashed with ESP32 Marauder. This app is the host side of that UART
 * link: it starts Marauder's `scanall`, parses the output
 * (marauder_proto.c), and shows a live, sorted list of networks with
 * signal, channel, encryption and connected-device count.
 *
 * Launched from GPIO > Wi-Fi Scanner rather than its own main-menu tile.
 *
 * Results can be logged to SD for the App's capture.pull path.
 */
#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <gui/elements.h>
#include <expansion/expansion.h>
#include <input/input.h>
#include <storage/storage.h>
#include <string.h>

#include "marauder_proto.h"

#define TAG "WifiScanner"

#define WS_BAUD         115200
#define WS_RX_STREAM    2048
#define WS_LINE_MAX     160
#define WS_MAX_APS      48
#define WS_MAX_STATIONS 96
#define WS_PROBE_MS     3000 // resend `info` this often until the board answers
#define WS_ROWS         5

#define WS_LOG_DIR EXT_PATH("wifi_scan")

typedef enum {
    WsCmdToggleScan,
    WsCmdRescan,
} WsCmd;

typedef enum {
    WsBoardUnknown, // probing
    WsBoardMissing, // no reply yet
    WsBoardReady,
} WsBoard;

typedef struct {
    MpAp ap;
    uint8_t clients;
    uint32_t first_seen;
} WsNetwork;

typedef struct {
    WsBoard board;
    char version[24];
    bool scanning;
    bool logging;
    bool detail; // showing the selected network's details

    WsNetwork nets[WS_MAX_APS];
    uint8_t count;
    uint8_t order[WS_MAX_APS]; // indices into nets, strongest first
    uint8_t selected; // position in order
    uint8_t scroll;
} WsModel;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    View* list_view;

    Expansion* expansion;
    FuriHalSerialHandle* serial;
    FuriStreamBuffer* rx_stream;
    FuriMessageQueue* cmd_queue;
    FuriThread* worker;
    volatile bool worker_run;
    bool otg_enabled_by_us;

    // Station -> AP pairs already counted. Worker thread only.
    uint8_t sta_mac[WS_MAX_STATIONS][6];
    uint8_t sta_ap[WS_MAX_STATIONS];
    uint8_t sta_count;

    FuriMutex* log_mutex; // guards log_file/logging: worker vs input thread
    Storage* storage;
    File* log_file;
    bool logging;
} WsApp;

// ---- UART -------------------------------------------------------------------

static void ws_serial_rx_cb(FuriHalSerialHandle* handle, FuriHalSerialRxEvent event, void* ctx) {
    WsApp* app = ctx;
    if(event & FuriHalSerialRxEventData) {
        while(furi_hal_serial_async_rx_available(handle)) {
            uint8_t c = furi_hal_serial_async_rx(handle);
            furi_stream_buffer_send(app->rx_stream, &c, 1, 0);
        }
    }
}

static void ws_send(WsApp* app, const char* line) {
    furi_hal_serial_tx(app->serial, (const uint8_t*)line, strlen(line));
    furi_hal_serial_tx_wait_complete(app->serial);
}

// ---- SD logging (callers hold log_mutex) ------------------------------------

static bool ws_log_start(WsApp* app) {
    storage_common_mkdir(app->storage, WS_LOG_DIR);
    FuriString* path =
        furi_string_alloc_printf("%s/scan_%lu.csv", WS_LOG_DIR, (unsigned long)furi_get_tick());
    bool ok = storage_file_open(
        app->log_file, furi_string_get_cstr(path), FSAM_WRITE, FSOM_CREATE_ALWAYS);
    furi_string_free(path);
    if(ok) {
        const char* hdr = "tick_ms,bssid,ssid,hidden,channel,rssi_dbm,encrypted\n";
        storage_file_write(app->log_file, hdr, strlen(hdr));
        app->logging = true;
    } else {
        storage_file_close(app->log_file); // required even on failed open
    }
    return ok;
}

static void ws_log_stop(WsApp* app) {
    if(!app->logging) return;
    storage_file_close(app->log_file);
    app->logging = false;
}

static void ws_log_network(WsApp* app, const MpAp* ap) {
    furi_mutex_acquire(app->log_mutex, FuriWaitForever);
    if(app->logging) {
        // CSV-quote the SSID: it is free text and may hold commas or quotes.
        char ssid[MP_SSID_MAX * 2 + 3];
        size_t n = 0;
        ssid[n++] = '"';
        for(const char* c = ap->ssid; *c && n < sizeof(ssid) - 2; c++) {
            if(*c == '"') ssid[n++] = '"';
            ssid[n++] = *c;
        }
        ssid[n++] = '"';
        ssid[n] = 0;
        char line[128];
        int len = snprintf(
            line,
            sizeof(line),
            "%lu,%02x:%02x:%02x:%02x:%02x:%02x,%s,%u,%u,%d,%u\n",
            (unsigned long)furi_get_tick(),
            ap->bssid[0],
            ap->bssid[1],
            ap->bssid[2],
            ap->bssid[3],
            ap->bssid[4],
            ap->bssid[5],
            ssid,
            ap->hidden,
            ap->channel,
            ap->rssi,
            ap->encrypted);
        if(len > 0) storage_file_write(app->log_file, line, len);
    }
    furi_mutex_release(app->log_mutex);
}

// ---- model updates (worker thread) ------------------------------------------

static void ws_sort(WsModel* m) {
    // Insertion sort by RSSI; the list is small and nearly sorted.
    for(uint8_t i = 0; i < m->count; i++)
        m->order[i] = i;
    for(uint8_t i = 1; i < m->count; i++) {
        uint8_t v = m->order[i];
        int j = i - 1;
        while(j >= 0 && m->nets[m->order[j]].ap.rssi < m->nets[v].ap.rssi) {
            m->order[j + 1] = m->order[j];
            j--;
        }
        m->order[j + 1] = v;
    }
}

static int ws_find(const WsModel* m, const uint8_t bssid[6]) {
    for(uint8_t i = 0; i < m->count; i++) {
        if(memcmp(m->nets[i].ap.bssid, bssid, 6) == 0) return i;
    }
    return -1;
}

static void ws_on_ap(WsApp* app, const MpAp* ap) {
    bool is_new = false;
    with_view_model(
        app->list_view,
        WsModel * m,
        {
            int i = ws_find(m, ap->bssid);
            if(i >= 0) {
                m->nets[i].ap.rssi = ap->rssi; // repeat sightings refresh signal
                m->nets[i].ap.channel = ap->channel;
            } else if(m->count < WS_MAX_APS) {
                m->nets[m->count].ap = *ap;
                m->nets[m->count].clients = 0;
                m->nets[m->count].first_seen = furi_get_tick();
                m->count++;
                is_new = true;
            }
            // Keep the cursor on the same network while the order shifts.
            uint8_t sel_net = m->count ? m->order[m->selected] : 0;
            ws_sort(m);
            for(uint8_t k = 0; k < m->count; k++) {
                if(m->order[k] == sel_net) m->selected = k;
            }
        },
        true);
    if(is_new) ws_log_network(app, ap);
}

static void ws_on_station(WsApp* app, const MpStation* st) {
    if(mp_mac_is_group(st->station)) return;
    int ap_index = -1;
    with_view_model(app->list_view, WsModel * m, { ap_index = ws_find(m, st->ap); }, false);
    if(ap_index < 0) return;

    for(uint8_t i = 0; i < app->sta_count; i++) {
        if(app->sta_ap[i] == ap_index && memcmp(app->sta_mac[i], st->station, 6) == 0) return;
    }
    if(app->sta_count >= WS_MAX_STATIONS) return;
    memcpy(app->sta_mac[app->sta_count], st->station, 6);
    app->sta_ap[app->sta_count++] = ap_index;
    with_view_model(app->list_view, WsModel * m, { m->nets[ap_index].clients++; }, true);
}

static void ws_handle_line(WsApp* app, const char* line) {
    MpLine l;
    if(!mp_parse_line(line, &l)) return;
    switch(l.type) {
    case MpLineAp:
        ws_on_ap(app, &l.ap);
        break;
    case MpLineStation:
        ws_on_station(app, &l.station);
        break;
    case MpLineVersion:
        with_view_model(
            app->list_view,
            WsModel * m,
            {
                strlcpy(m->version, l.text, sizeof(m->version));
                m->board = WsBoardReady;
            },
            true);
        break;
    default:
        break;
    }
}

// ---- worker -------------------------------------------------------------------

static void ws_start_scan(WsApp* app) {
    // Hopping must be on or the radio stays on one channel (it shipped off
    // on this board). The setting persists on the module; re-asserting it
    // is harmless.
    ws_send(app, "settings -s ChanHop enable\n");
    ws_send(app, "scanall\n");
    with_view_model(app->list_view, WsModel * m, { m->scanning = true; }, true);
}

static void ws_stop_scan(WsApp* app) {
    ws_send(app, "stopscan\n");
    with_view_model(app->list_view, WsModel * m, { m->scanning = false; }, true);
}

static void ws_clear(WsApp* app) {
    app->sta_count = 0;
    with_view_model(
        app->list_view,
        WsModel * m,
        {
            m->count = 0;
            m->selected = 0;
            m->scroll = 0;
            m->detail = false;
        },
        true);
}

static int32_t ws_worker(void* ctx) {
    WsApp* app = ctx;
    char line[WS_LINE_MAX];
    size_t len = 0;
    uint8_t buf[64];
    bool ready = false;
    uint32_t last_probe = 0;

    // Leave any scan a previous session started, then identify the board.
    ws_send(app, "stopscan\n");

    while(app->worker_run) {
        uint32_t now = furi_get_tick();
        if(!ready && (last_probe == 0 || now - last_probe >= furi_ms_to_ticks(WS_PROBE_MS))) {
            if(last_probe) {
                with_view_model(app->list_view, WsModel * m, { m->board = WsBoardMissing; }, true);
            }
            ws_send(app, "info\n");
            last_probe = now;
        }

        WsCmd cmd;
        while(ready && furi_message_queue_get(app->cmd_queue, &cmd, 0) == FuriStatusOk) {
            bool scanning = false;
            with_view_model(app->list_view, WsModel * m, { scanning = m->scanning; }, false);
            if(cmd == WsCmdToggleScan) {
                scanning ? ws_stop_scan(app) : ws_start_scan(app);
            } else if(cmd == WsCmdRescan) {
                if(scanning) ws_stop_scan(app);
                ws_send(app, "clearlist -a\n");
                ws_send(app, "clearlist -c\n");
                ws_clear(app);
                ws_start_scan(app);
            }
        }

        size_t got = furi_stream_buffer_receive(app->rx_stream, buf, sizeof(buf), 50);
        for(size_t i = 0; i < got; i++) {
            char c = (char)buf[i];
            if(c == '\r') continue;
            if(c == '\n') {
                line[len] = 0;
                ws_handle_line(app, line);
                len = 0;
            } else if(len < sizeof(line) - 1) {
                line[len++] = c;
            } else {
                len = 0; // overlong line: drop it rather than parse half
            }
        }

        if(!ready) {
            with_view_model(
                app->list_view, WsModel * m, { ready = m->board == WsBoardReady; }, false);
            if(ready) ws_start_scan(app); // scan as soon as the board answers
        }
    }

    if(ready) ws_send(app, "stopscan\n");
    return 0;
}

static void ws_send_cmd(WsApp* app, WsCmd cmd) {
    furi_message_queue_put(app->cmd_queue, &cmd, 0);
}

// ---- drawing ------------------------------------------------------------------

// 0-4 bars from RSSI, roughly the thresholds phones use.
static uint8_t ws_bars(int8_t rssi) {
    if(rssi >= -55) return 4;
    if(rssi >= -67) return 3;
    if(rssi >= -78) return 2;
    if(rssi >= -88) return 1;
    return 0;
}

static void ws_draw_bars(Canvas* canvas, int x, int y_bottom, uint8_t bars) {
    for(uint8_t i = 0; i < 4; i++) {
        int h = 2 + i * 2;
        if(i < bars) {
            canvas_draw_box(canvas, x + i * 3, y_bottom - h, 2, h);
        } else {
            canvas_draw_dot(canvas, x + i * 3, y_bottom - 1);
        }
    }
}

static void ws_draw_lock(Canvas* canvas, int x, int y_bottom) {
    canvas_draw_frame(canvas, x + 1, y_bottom - 7, 3, 3);
    canvas_draw_box(canvas, x, y_bottom - 5, 5, 4);
}

static void ws_ssid_label(const MpAp* ap, char* out, size_t size) {
    if(ap->hidden) {
        snprintf(out, size, "<hidden %02X%02X>", ap->bssid[4], ap->bssid[5]);
    } else {
        strlcpy(out, ap->ssid, size);
    }
}

static void ws_detail_draw(Canvas* canvas, const WsModel* m) {
    const WsNetwork* n = &m->nets[m->order[m->selected]];
    char buf[48];

    canvas_set_font(canvas, FontPrimary);
    ws_ssid_label(&n->ap, buf, 22);
    canvas_draw_str(canvas, 0, 10, buf);

    canvas_set_font(canvas, FontSecondary);
    const uint8_t* b = n->ap.bssid;
    snprintf(
        buf, sizeof(buf), "BSSID %02X:%02X:%02X:%02X:%02X:%02X", b[0], b[1], b[2], b[3], b[4], b[5]);
    canvas_draw_str(canvas, 0, 22, buf);
    snprintf(buf, sizeof(buf), "Signal %d dBm", n->ap.rssi);
    canvas_draw_str(canvas, 0, 32, buf);
    ws_draw_bars(canvas, 70, 33, ws_bars(n->ap.rssi));
    snprintf(
        buf, sizeof(buf), "Channel %u  (%s GHz)", n->ap.channel, n->ap.channel <= 14 ? "2.4" : "5");
    canvas_draw_str(canvas, 0, 42, buf);
    canvas_draw_str(canvas, 0, 52, n->ap.encrypted ? "Encrypted" : "Open (no encryption)");
    snprintf(buf, sizeof(buf), "Devices seen: %u", n->clients);
    canvas_draw_str(canvas, 0, 62, buf);
}

static void ws_list_draw(Canvas* canvas, void* model) {
    WsModel* m = model;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontSecondary);

    if(m->board != WsBoardReady) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(
            canvas,
            64,
            14,
            AlignCenter,
            AlignCenter,
            m->board == WsBoardMissing ? "No Marauder board" : "Looking for board...");
        canvas_set_font(canvas, FontSecondary);
        if(m->board == WsBoardMissing) {
            canvas_draw_str_aligned(
                canvas, 64, 30, AlignCenter, AlignCenter, "Seat the Wi-Fi devboard");
            canvas_draw_str_aligned(
                canvas, 64, 40, AlignCenter, AlignCenter, "on GPIO, running ESP32");
            canvas_draw_str_aligned(
                canvas, 64, 50, AlignCenter, AlignCenter, "Marauder (pins 13/14)");
        }
        return;
    }

    if(m->detail && m->count) {
        ws_detail_draw(canvas, m);
        return;
    }

    char buf[40];
    snprintf(buf, sizeof(buf), "%u network%s", m->count, m->count == 1 ? "" : "s");
    canvas_draw_str(canvas, 0, 7, buf);
    const char* state = m->scanning ? (m->logging ? "SCAN REC" : "SCANNING") :
                                      (m->logging ? "PAUSED REC" : "PAUSED");
    canvas_draw_str_aligned(canvas, 127, 7, AlignRight, AlignBottom, state);
    canvas_draw_line(canvas, 0, 9, 127, 9);

    if(m->count == 0) {
        canvas_draw_str_aligned(
            canvas,
            64,
            34,
            AlignCenter,
            AlignCenter,
            m->scanning ? "Listening on all channels" : "Left: start scan");
        return;
    }

    for(uint8_t row = 0; row < WS_ROWS; row++) {
        uint8_t pos = m->scroll + row;
        if(pos >= m->count) break;
        const WsNetwork* n = &m->nets[m->order[pos]];
        int y = 19 + row * 10; // text baseline
        bool sel = pos == m->selected;
        if(sel) {
            canvas_draw_box(canvas, 0, y - 8, 124, 10);
            canvas_set_color(canvas, ColorWhite);
        }
        ws_draw_bars(canvas, 2, y + 1, ws_bars(n->ap.rssi));
        if(n->ap.encrypted) ws_draw_lock(canvas, 15, y + 1);
        ws_ssid_label(&n->ap, buf, 18);
        canvas_draw_str(canvas, 22, y, buf);
        snprintf(buf, sizeof(buf), "%u", n->ap.channel);
        canvas_draw_str_aligned(canvas, 122, y, AlignRight, AlignBottom, buf);
        canvas_set_color(canvas, ColorBlack);
    }
    elements_scrollbar_pos(canvas, 127, 11, 52, m->selected, m->count);
}

// ---- input ----------------------------------------------------------------------

static bool ws_list_input(InputEvent* event, void* ctx) {
    WsApp* app = ctx;

    if(event->type == InputTypeLong && event->key == InputKeyOk) {
        furi_mutex_acquire(app->log_mutex, FuriWaitForever);
        bool now = app->logging ? (ws_log_stop(app), false) : ws_log_start(app);
        furi_mutex_release(app->log_mutex);
        // Networks already found go into the new log too.
        if(now) {
            WsNetwork snapshot[WS_MAX_APS];
            uint8_t count = 0;
            with_view_model(
                app->list_view,
                WsModel * m,
                {
                    count = m->count;
                    memcpy(snapshot, m->nets, sizeof(WsNetwork) * count);
                },
                false);
            for(uint8_t i = 0; i < count; i++)
                ws_log_network(app, &snapshot[i].ap);
        }
        with_view_model(app->list_view, WsModel * m, { m->logging = now; }, true);
        return true;
    }
    if(event->type != InputTypeShort && event->type != InputTypeRepeat) return false;

    bool handled = true;
    bool in_detail = false;
    with_view_model(
        app->list_view,
        WsModel * m,
        {
            in_detail = m->detail;
            switch(event->key) {
            case InputKeyUp:
                if(m->selected > 0) m->selected--;
                break;
            case InputKeyDown:
                if(m->selected + 1 < m->count) m->selected++;
                break;
            case InputKeyOk:
                m->detail = !m->detail && m->count > 0;
                break;
            case InputKeyBack:
                // Back closes the details first; from the list it exits.
                handled = m->detail;
                m->detail = false;
                break;
            default:
                handled = false;
                break;
            }
            if(m->selected < m->scroll) m->scroll = m->selected;
            if(m->selected >= m->scroll + WS_ROWS) m->scroll = m->selected - WS_ROWS + 1;
        },
        true);

    if(in_detail) return handled;
    if(event->type == InputTypeShort && event->key == InputKeyLeft) {
        ws_send_cmd(app, WsCmdToggleScan);
        handled = true;
    } else if(event->type == InputTypeShort && event->key == InputKeyRight) {
        ws_send_cmd(app, WsCmdRescan);
        handled = true;
    }
    return handled;
}

static uint32_t ws_exit(void* ctx) {
    UNUSED(ctx);
    return VIEW_NONE;
}

// ---- lifecycle --------------------------------------------------------------------

static WsApp* ws_alloc(void) {
    WsApp* app = malloc(sizeof(WsApp));
    memset(app, 0, sizeof(*app));

    // The expansion service listens on this UART for modules; it must let go
    // before the port is acquired (see expansion.h). On this device it was
    // set to listen on USART - exactly the devboard's pins.
    app->expansion = furi_record_open(RECORD_EXPANSION);
    expansion_disable(app->expansion);

    // The devboard is powered from GPIO pin 1 (5V) when not on USB.
    if(!furi_hal_power_is_otg_enabled()) {
        app->otg_enabled_by_us = furi_hal_power_enable_otg();
    }

    app->rx_stream = furi_stream_buffer_alloc(WS_RX_STREAM, 1);
    app->cmd_queue = furi_message_queue_alloc(4, sizeof(WsCmd));
    app->serial = furi_hal_serial_control_acquire(FuriHalSerialIdUsart);
    furi_check(app->serial);
    furi_hal_serial_init(app->serial, WS_BAUD);
    furi_hal_serial_async_rx_start(app->serial, ws_serial_rx_cb, app, false);

    app->log_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->storage = furi_record_open(RECORD_STORAGE);
    app->log_file = storage_file_alloc(app->storage);

    app->gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();

    app->list_view = view_alloc();
    view_set_context(app->list_view, app);
    view_set_draw_callback(app->list_view, ws_list_draw);
    view_set_input_callback(app->list_view, ws_list_input);
    view_set_previous_callback(app->list_view, ws_exit);
    view_allocate_model(app->list_view, ViewModelTypeLocking, sizeof(WsModel));
    with_view_model(app->list_view, WsModel * m, { memset(m, 0, sizeof(*m)); }, false);
    view_dispatcher_add_view(app->view_dispatcher, 0, app->list_view);

    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_switch_to_view(app->view_dispatcher, 0);

    app->worker_run = true;
    app->worker = furi_thread_alloc_ex("WifiScannerWorker", 2048, ws_worker, app);
    furi_thread_start(app->worker);
    return app;
}

static void ws_free(WsApp* app) {
    app->worker_run = false;
    furi_thread_join(app->worker);
    furi_thread_free(app->worker);

    furi_hal_serial_async_rx_stop(app->serial);
    furi_hal_serial_deinit(app->serial);
    furi_hal_serial_control_release(app->serial);
    furi_stream_buffer_free(app->rx_stream);
    furi_message_queue_free(app->cmd_queue);

    if(app->otg_enabled_by_us) furi_hal_power_disable_otg();
    expansion_enable(app->expansion);
    furi_record_close(RECORD_EXPANSION);

    furi_mutex_acquire(app->log_mutex, FuriWaitForever);
    ws_log_stop(app);
    furi_mutex_release(app->log_mutex);
    storage_file_free(app->log_file);
    furi_mutex_free(app->log_mutex);
    furi_record_close(RECORD_STORAGE);

    view_dispatcher_remove_view(app->view_dispatcher, 0);
    view_free(app->list_view);
    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_GUI);
    free(app);
}

int32_t wifi_scanner_app(void* args) {
    UNUSED(args);
    WsApp* app = ws_alloc();
    view_dispatcher_run(app->view_dispatcher);
    ws_free(app);
    return 0;
}
