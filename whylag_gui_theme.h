#ifndef WHYLAG_GUI_THEME_H
#define WHYLAG_GUI_THEME_H

#include <windows.h>
#include <commctrl.h>

/* Palette - tweak here for rapid visual iteration */
#define WL_CLR_BG            RGB(30, 30, 30)
#define WL_CLR_PANEL         RGB(37, 37, 40)
#define WL_CLR_HEADER        RGB(45, 45, 48)
#define WL_CLR_TEXT          RGB(235, 235, 235)
#define WL_CLR_MUTED         RGB(160, 160, 165)
#define WL_CLR_BORDER        RGB(62, 62, 68)
#define WL_CLR_ACCENT        RGB(0, 120, 212)
#define WL_CLR_ACCENT_HOVER  RGB(26, 145, 235)
#define WL_CLR_BTN           RGB(55, 55, 60)
#define WL_CLR_BTN_HOVER     RGB(68, 68, 74)
#define WL_CLR_BTN_BORDER    RGB(85, 85, 92)
#define WL_CLR_BTN_DISABLED  RGB(42, 42, 46)
#define WL_CLR_EDIT          RGB(48, 48, 52)
#define WL_CLR_OK            RGB(76, 175, 80)
#define WL_CLR_WARN          RGB(255, 185, 0)
#define WL_CLR_BAD           RGB(244, 67, 54)

typedef enum {
    WL_BTN_NORMAL = 0,
    WL_BTN_PRIMARY,
    WL_BTN_TAB,
    WL_BTN_TAB_ACTIVE
} WlBtnStyle;

void wl_theme_init(void);
void wl_theme_shutdown(void);
HFONT wl_font_ui(void);
HFONT wl_font_title(void);
HFONT wl_font_mono(void);
HBRUSH wl_brush(int which); /* 0=bg 1=panel 2=header 3=edit */

void wl_fill_rect(HDC hdc, const RECT *rc, COLORREF fill, COLORREF border);
void wl_draw_button(LPDRAWITEMSTRUCT dis, WlBtnStyle style, const char *label, HFONT font);
void wl_draw_checkbox(LPDRAWITEMSTRUCT dis, const char *label, HFONT font);
void wl_draw_static_text(HDC hdc, HWND hwnd, HFONT font, COLORREF text, HBRUSH bg);
int wl_header_custom_draw(LPNMHDR nm, HFONT font);
void wl_track_mouse_leave(HWND hwnd);

#endif
