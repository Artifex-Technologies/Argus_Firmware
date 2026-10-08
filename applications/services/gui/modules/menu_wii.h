/**
 * @file menu_wii.h
 * GUI: Wii-style grid menu view module API
 *
 * Drop-in replacement for the stock Menu module, rendering a 3x2 grid of
 * tiles instead of a vertical list. Mirrors the Menu API so the only change
 * at a call site is the type and function prefix.
 */

#pragma once

#include <gui/view.h>

#ifdef __cplusplus
extern "C" {
#endif

/** MenuWii anonymous structure */
typedef struct MenuWii MenuWii;

/** MenuWii Item Callback */
typedef void (*MenuWiiItemCallback)(void* context, uint32_t index);

/** MenuWii allocation and initialization
 *
 * @return     MenuWii instance
 */
MenuWii* menu_wii_alloc(void);

/** Free menu
 *
 * @param      menu  MenuWii instance
 */
void menu_wii_free(MenuWii* menu);

/** Get MenuWii view
 *
 * @param      menu  MenuWii instance
 *
 * @return     View instance
 */
View* menu_wii_get_view(MenuWii* menu);

/** Add item to menu
 *
 * @param      menu      MenuWii instance
 * @param      label     menu item string label
 * @param      icon      Icon instance
 * @param      index     menu item index
 * @param      callback  MenuWiiItemCallback instance
 * @param      context   pointer to context
 */
void menu_wii_add_item(
    MenuWii* menu,
    const char* label,
    const Icon* icon,
    uint32_t index,
    MenuWiiItemCallback callback,
    void* context);

/** Clean menu
 * @note       this function does not free menu instance
 *
 * @param      menu  MenuWii instance
 */
void menu_wii_reset(MenuWii* menu);

/** Set current menu item
 *
 * @param      menu   MenuWii instance
 * @param      index  The index
 */
void menu_wii_set_selected_item(MenuWii* menu, uint32_t index);

#ifdef __cplusplus
}
#endif
