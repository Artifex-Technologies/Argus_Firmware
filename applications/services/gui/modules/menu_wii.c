#include "menu_wii.h"

#include <gui/elements.h>
#include <assets_icons.h>
#include <furi.h>
#include <m-array.h>
#include <string.h>

// 3 columns x 2 rows of tiles on a 128x64 screen.
#define MENU_WII_CELLS     6u
#define MENU_WII_CELL_W    40
#define MENU_WII_CELL_H    30
#define MENU_WII_COL_PITCH 43
#define MENU_WII_ROW_PITCH 32
// Icons are centred in the upper band of a tile; the label sits below them.
#define MENU_WII_ICON_H    20
#define MENU_WII_LABEL_Y   26
#define MENU_WII_LABEL_W   36

struct MenuWii {
    View* view;
};

typedef struct {
    const char* label;
    IconAnimation* icon;
    uint32_t index;
    MenuWiiItemCallback callback;
    void* callback_context;
} MenuWiiItem;

ARRAY_DEF(MenuWiiItemArray, MenuWiiItem, M_POD_OPLIST); //-V658

#define M_OPL_MenuWiiItemArray_t() ARRAY_OPLIST(MenuWiiItemArray, M_POD_OPLIST)

typedef struct {
    MenuWiiItemArray_t items;
    size_t position;
} MenuWiiModel;

static void menu_wii_process_up(MenuWii* menu);
static void menu_wii_process_down(MenuWii* menu);
static void menu_wii_process_left(MenuWii* menu);
static void menu_wii_process_right(MenuWii* menu);
static void menu_wii_process_ok(MenuWii* menu);

static void menu_wii_draw_icon(Canvas* canvas, MenuWiiItem* item, int32_t x, int32_t y) {
    if(!item->icon) return;
    const int32_t w = icon_animation_get_width(item->icon);
    const int32_t h = icon_animation_get_height(item->icon);
    canvas_draw_icon_animation(
        canvas, x + (MENU_WII_CELL_W - w) / 2, y + (MENU_WII_ICON_H - h) / 2, item->icon);
}

static void menu_wii_draw_label(Canvas* canvas, const char* label, int32_t centre_x, int32_t y) {
    if(canvas_string_width(canvas, label) <= MENU_WII_LABEL_W) {
        canvas_draw_str_aligned(canvas, centre_x, y, AlignCenter, AlignBottom, label);
        return;
    }

    // A tile is too narrow to scroll long labels the way the list menu does,
    // so clip to what fits rather than bleeding into the neighbouring tile.
    char clipped[24];
    size_t len = strlen(label);
    if(len >= sizeof(clipped)) len = sizeof(clipped) - 1;
    memcpy(clipped, label, len);
    clipped[len] = '\0';

    while(len > 1 && canvas_string_width(canvas, clipped) > MENU_WII_LABEL_W) {
        clipped[--len] = '\0';
    }

    canvas_draw_str_aligned(canvas, centre_x, y, AlignCenter, AlignBottom, clipped);
}

static void menu_wii_draw_callback(Canvas* canvas, void* _model) {
    MenuWiiModel* model = _model;

    canvas_clear(canvas);

    const size_t position = model->position;
    const size_t items_count = MenuWiiItemArray_size(model->items);

    if(!items_count) {
        canvas_draw_str(canvas, 2, 32, "Empty");
        return;
    }

    // Page the grid in column-pairs so the selection stays on screen without
    // the tiles jumping a whole page at a time.
    size_t shift_position;
    if(items_count > MENU_WII_CELLS && position >= 4) {
        if(position >= items_count - 2 + (items_count % 2)) {
            shift_position = position - (position % 2) - 4;
        } else {
            shift_position = position - (position % 2) - 2;
        }
    } else {
        shift_position = 0;
    }

    canvas_set_font(canvas, FontSecondary);

    for(size_t i = 0; i < MENU_WII_CELLS; i++) {
        const size_t item_i = shift_position + i;
        if(item_i >= items_count) continue;

        const int32_t x = (int32_t)(i / 2) * MENU_WII_COL_PITCH + 1;
        const int32_t y = (int32_t)(i % 2) * MENU_WII_ROW_PITCH;
        const bool selected = (item_i == position);

        if(selected) {
            elements_slightly_rounded_box(canvas, x, y, MENU_WII_CELL_W, MENU_WII_CELL_H);
            canvas_set_color(canvas, ColorWhite);
        }

        MenuWiiItem* item = MenuWiiItemArray_get(model->items, item_i);
        menu_wii_draw_icon(canvas, item, x, y);
        menu_wii_draw_label(canvas, item->label, x + MENU_WII_CELL_W / 2, y + MENU_WII_LABEL_Y);

        if(selected) {
            canvas_set_color(canvas, ColorBlack);
        } else {
            elements_frame(canvas, x, y, MENU_WII_CELL_W, MENU_WII_CELL_H);
        }
    }
}

static bool menu_wii_input_callback(InputEvent* event, void* context) {
    MenuWii* menu = context;
    bool consumed = false;

    if(event->type == InputTypeShort || event->type == InputTypeRepeat) {
        if(event->key == InputKeyUp) {
            consumed = true;
            menu_wii_process_up(menu);
        } else if(event->key == InputKeyDown) {
            consumed = true;
            menu_wii_process_down(menu);
        } else if(event->key == InputKeyLeft) {
            consumed = true;
            menu_wii_process_left(menu);
        } else if(event->key == InputKeyRight) {
            consumed = true;
            menu_wii_process_right(menu);
        } else if(event->key == InputKeyOk && event->type == InputTypeShort) {
            consumed = true;
            menu_wii_process_ok(menu);
        }
    }

    return consumed;
}

static void menu_wii_enter(void* context) {
    MenuWii* menu = context;
    with_view_model(
        menu->view,
        MenuWiiModel * model,
        {
            if(MenuWiiItemArray_size(model->items)) {
                MenuWiiItem* item = MenuWiiItemArray_get(model->items, model->position);
                icon_animation_start(item->icon);
            }
        },
        false);
}

static void menu_wii_exit(void* context) {
    MenuWii* menu = context;
    with_view_model(
        menu->view,
        MenuWiiModel * model,
        {
            if(MenuWiiItemArray_size(model->items)) {
                MenuWiiItem* item = MenuWiiItemArray_get(model->items, model->position);
                icon_animation_stop(item->icon);
            }
        },
        false);
}

MenuWii* menu_wii_alloc(void) {
    MenuWii* menu = malloc(sizeof(MenuWii));
    menu->view = view_alloc();
    view_set_context(menu->view, menu);
    view_allocate_model(menu->view, ViewModelTypeLocking, sizeof(MenuWiiModel));
    view_set_draw_callback(menu->view, menu_wii_draw_callback);
    view_set_input_callback(menu->view, menu_wii_input_callback);
    view_set_enter_callback(menu->view, menu_wii_enter);
    view_set_exit_callback(menu->view, menu_wii_exit);

    with_view_model(
        menu->view,
        MenuWiiModel * model,
        {
            MenuWiiItemArray_init(model->items);
            model->position = 0;
        },
        true);

    return menu;
}

void menu_wii_free(MenuWii* menu) {
    furi_check(menu);

    menu_wii_reset(menu);
    with_view_model(
        menu->view, MenuWiiModel * model, { MenuWiiItemArray_clear(model->items); }, false);
    view_free(menu->view);

    free(menu);
}

View* menu_wii_get_view(MenuWii* menu) {
    furi_check(menu);
    return menu->view;
}

void menu_wii_add_item(
    MenuWii* menu,
    const char* label,
    const Icon* icon,
    uint32_t index,
    MenuWiiItemCallback callback,
    void* context) {
    furi_check(menu);
    furi_check(label);

    MenuWiiItem* item = NULL;
    with_view_model(
        menu->view,
        MenuWiiModel * model,
        {
            item = MenuWiiItemArray_push_new(model->items);
            item->label = label;
            item->icon = icon ? icon_animation_alloc(icon) : icon_animation_alloc(&A_Plugins_14);
            view_tie_icon_animation(menu->view, item->icon);
            item->index = index;
            item->callback = callback;
            item->callback_context = context;
        },
        true);
}

void menu_wii_reset(MenuWii* menu) {
    furi_check(menu);
    with_view_model(
        menu->view,
        MenuWiiModel * model,
        {
            for
                M_EACH(item, model->items, MenuWiiItemArray_t) {
                    icon_animation_stop(item->icon);
                    icon_animation_free(item->icon);
                }

            MenuWiiItemArray_reset(model->items);
            model->position = 0;
        },
        true);
}

void menu_wii_set_selected_item(MenuWii* menu, uint32_t index) {
    furi_check(menu);

    with_view_model(
        menu->view,
        MenuWiiModel * model,
        {
            if(index < MenuWiiItemArray_size(model->items)) {
                model->position = index;
            }
        },
        true);
}

static void menu_wii_set_position(MenuWii* menu, size_t position) {
    with_view_model(
        menu->view,
        MenuWiiModel * model,
        {
            if(MenuWiiItemArray_size(model->items)) {
                MenuWiiItem* item = MenuWiiItemArray_get(model->items, model->position);
                icon_animation_stop(item->icon);

                model->position = position;

                item = MenuWiiItemArray_get(model->items, model->position);
                icon_animation_start(item->icon);
            }
        },
        true);
}

/** Read the current position and item count, returning false for a grid too
 * small to navigate. Guards the caller against size_t underflow when a single
 * item would otherwise be stepped past.
 */
static bool menu_wii_navigable(MenuWii* menu, size_t* position, size_t* count) {
    bool navigable = false;
    with_view_model(
        menu->view,
        MenuWiiModel * model,
        {
            *count = MenuWiiItemArray_size(model->items);
            *position = model->position;
            navigable = (*count > 1);
        },
        false);
    return navigable;
}

// Up and Down both toggle between the two rows of the current column.
static void menu_wii_process_vertical(MenuWii* menu) {
    size_t position, count;
    if(!menu_wii_navigable(menu, &position, &count)) return;

    if(position % 2 || (position == count - 1 && count % 2)) {
        position--;
    } else {
        position++;
    }

    menu_wii_set_position(menu, position);
}

static void menu_wii_process_up(MenuWii* menu) {
    menu_wii_process_vertical(menu);
}

static void menu_wii_process_down(MenuWii* menu) {
    menu_wii_process_vertical(menu);
}

static void menu_wii_process_left(MenuWii* menu) {
    size_t position, count;
    if(!menu_wii_navigable(menu, &position, &count)) return;

    if(position < 2) {
        if(count % 2) {
            position = count - 1;
        } else {
            position = count - 2 + position % 2;
        }
    } else {
        position -= 2;
    }

    menu_wii_set_position(menu, position);
}

static void menu_wii_process_right(MenuWii* menu) {
    size_t position, count;
    if(!menu_wii_navigable(menu, &position, &count)) return;

    if(count % 2) {
        if(position == count - 1) {
            position = 0;
        } else if(position == count - 2) {
            position = count - 1;
        } else {
            position += 2;
        }
    } else {
        position += 2;
        if(position >= count) {
            position = position % 2;
        }
    }

    menu_wii_set_position(menu, position);
}

static void menu_wii_process_ok(MenuWii* menu) {
    MenuWiiItem* item = NULL;
    with_view_model(
        menu->view,
        MenuWiiModel * model,
        {
            if(MenuWiiItemArray_size(model->items)) {
                item = MenuWiiItemArray_get(model->items, model->position);
            }
        },
        true);
    if(item && item->callback) {
        item->callback(item->callback_context, item->index);
    }
}
