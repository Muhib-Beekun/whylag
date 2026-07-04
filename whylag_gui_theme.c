#include "whylag_gui_theme.h"
#include <stdio.h>
#include <string.h>

static HFONT g_font_ui, g_font_title, g_font_mono;
static HBRUSH g_br_bg, g_br_panel, g_br_header, g_br_edit;

void wl_theme_init(void)
{
    g_font_ui = CreateFontA(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
    g_font_title = CreateFontA(-22, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
    g_font_mono = CreateFontA(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, "Consolas");
    g_br_bg = CreateSolidBrush(WL_CLR_BG);
    g_br_panel = CreateSolidBrush(WL_CLR_PANEL);
    g_br_header = CreateSolidBrush(WL_CLR_HEADER);
    g_br_edit = CreateSolidBrush(WL_CLR_EDIT);
}

void wl_theme_shutdown(void)
{
    DeleteObject(g_font_ui);
    DeleteObject(g_font_title);
    DeleteObject(g_font_mono);
    DeleteObject(g_br_bg);
    DeleteObject(g_br_panel);
    DeleteObject(g_br_header);
    DeleteObject(g_br_edit);
}

HFONT wl_font_ui(void) { return g_font_ui; }
HFONT wl_font_title(void) { return g_font_title; }
HFONT wl_font_mono(void) { return g_font_mono; }

HBRUSH wl_brush(int which)
{
    switch (which) {
    case 1: return g_br_panel;
    case 2: return g_br_header;
    case 3: return g_br_edit;
    default: return g_br_bg;
    }
}

void wl_fill_rect(HDC hdc, const RECT *rc, COLORREF fill, COLORREF border)
{
    HBRUSH br = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HBRUSH oldbr = SelectObject(hdc, br);
    HPEN oldpen = SelectObject(hdc, pen);
    Rectangle(hdc, rc->left, rc->top, rc->right, rc->bottom);
    SelectObject(hdc, oldbr);
    SelectObject(hdc, oldpen);
    DeleteObject(br);
    DeleteObject(pen);
}

static void wl_fill_round_rect(HDC hdc, const RECT *rc, COLORREF fill, COLORREF border, int radius)
{
    int w = rc->right - rc->left;
    int h = rc->bottom - rc->top;
    if (radius < 2) radius = 2;
    if (radius > h / 2) radius = h / 2;
    if (radius > w / 2) radius = w / 2;

    HRGN rgn = CreateRoundRectRgn(rc->left, rc->top, rc->right, rc->bottom, radius, radius);
    if (!rgn) {
        wl_fill_rect(hdc, rc, fill, border);
        return;
    }
    HBRUSH br = CreateSolidBrush(fill);
    FillRgn(hdc, rgn, br);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HBRUSH nullbr = (HBRUSH)GetStockObject(NULL_BRUSH);
    HBRUSH oldbr = SelectObject(hdc, nullbr);
    HPEN oldpen = SelectObject(hdc, pen);
    RoundRect(hdc, rc->left, rc->top, rc->right, rc->bottom, radius, radius);
    SelectObject(hdc, oldbr);
    SelectObject(hdc, oldpen);
    DeleteObject(br);
    DeleteObject(pen);
    DeleteObject(rgn);
}

void wl_draw_checkbox(LPDRAWITEMSTRUCT dis, const char *label, HFONT font)
{
    RECT r = dis->rcItem;
    BOOL checked = (SendMessage(dis->hwndItem, BM_GETCHECK, 0, 0) == BST_CHECKED);
    BOOL disabled = (dis->itemState & ODS_DISABLED) != 0;

    FillRect(dis->hDC, &r, wl_brush(0));

    int box = 16;
    int box_top = r.top + (r.bottom - r.top - box) / 2;
    RECT cb = { r.left, box_top, r.left + box, box_top + box };
    COLORREF border = disabled ? WL_CLR_BORDER : WL_CLR_BTN_BORDER;
    COLORREF fill = disabled ? WL_CLR_BTN_DISABLED : WL_CLR_EDIT;
    wl_fill_rect(dis->hDC, &cb, fill, border);

    if (checked) {
        HPEN pen = CreatePen(PS_SOLID, 2, disabled ? WL_CLR_MUTED : WL_CLR_ACCENT);
        HPEN old = SelectObject(dis->hDC, pen);
        MoveToEx(dis->hDC, cb.left + 3, cb.top + 8, NULL);
        LineTo(dis->hDC, cb.left + 6, cb.bottom - 4);
        LineTo(dis->hDC, cb.right - 2, cb.top + 3);
        SelectObject(dis->hDC, old);
        DeleteObject(pen);
    }

    RECT lr = r;
    lr.left = cb.right + 8;
    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, disabled ? WL_CLR_MUTED : WL_CLR_TEXT);
    SelectObject(dis->hDC, font);
    DrawTextA(dis->hDC, label, -1, &lr, DT_VCENTER | DT_SINGLELINE | DT_LEFT);
}

void wl_draw_static_text(HDC hdc, HWND hwnd, HFONT font, COLORREF text, HBRUSH bg)
{
    RECT r;
    GetClientRect(hwnd, &r);
    if (bg) FillRect(hdc, &r, bg);
    char buf[512];
    GetWindowTextA(hwnd, buf, sizeof(buf));
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, text);
    SelectObject(hdc, font);
    DrawTextA(hdc, buf, -1, &r, DT_VCENTER | DT_SINGLELINE | DT_LEFT);
}

void wl_draw_button(LPDRAWITEMSTRUCT dis, WlBtnStyle style, const char *label, HFONT font)
{
    RECT r = dis->rcItem;
    BOOL disabled = (dis->itemState & ODS_DISABLED) != 0;
    POINT pt;
    GetCursorPos(&pt);
    MapWindowPoints(NULL, dis->hwndItem, &pt, 1);
    BOOL hover = !disabled && PtInRect(&r, pt);
    BOOL pressed = hover && (GetAsyncKeyState(VK_LBUTTON) & 0x8000);

    COLORREF fill, border, text;
    if (disabled) {
        fill = WL_CLR_BTN_DISABLED; border = WL_CLR_BORDER; text = WL_CLR_MUTED;
    } else if (style == WL_BTN_PRIMARY) {
        fill = pressed ? WL_CLR_ACCENT : (hover ? WL_CLR_ACCENT_HOVER : WL_CLR_ACCENT);
        border = WL_CLR_ACCENT_HOVER; text = RGB(255, 255, 255);
    } else if (style == WL_BTN_TAB_ACTIVE) {
        fill = WL_CLR_PANEL; border = WL_CLR_PANEL; text = WL_CLR_TEXT;
    } else if (style == WL_BTN_TAB) {
        fill = hover ? WL_CLR_BTN_HOVER : WL_CLR_BG;
        border = WL_CLR_BG; text = WL_CLR_MUTED;
    } else {
        fill = pressed ? WL_CLR_BTN : (hover ? WL_CLR_BTN_HOVER : WL_CLR_BTN);
        border = WL_CLR_BTN_BORDER; text = WL_CLR_TEXT;
    }

    /* Clear default (white) button erase before painting. */
    FillRect(dis->hDC, &r, wl_brush(0));

    if (style == WL_BTN_TAB || style == WL_BTN_TAB_ACTIVE)
        wl_fill_rect(dis->hDC, &r, fill, border);
    else
        wl_fill_round_rect(dis->hDC, &r, fill, border, 6);

    if (style == WL_BTN_TAB_ACTIVE) {
        RECT accent = r;
        accent.top = accent.bottom - 3;
        HBRUSH ab = CreateSolidBrush(WL_CLR_ACCENT);
        FillRect(dis->hDC, &accent, ab);
        DeleteObject(ab);
    }

    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, text);
    SelectObject(dis->hDC, font);
    DrawTextA(dis->hDC, label, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    if (dis->itemState & ODS_FOCUS && style != WL_BTN_TAB && style != WL_BTN_TAB_ACTIVE) {
        RECT fr = r;
        InflateRect(&fr, -3, -3);
        DrawFocusRect(dis->hDC, &fr);
    }
}

int wl_header_custom_draw(LPNMHDR nm, HFONT font)
{
    if (nm->code != NM_CUSTOMDRAW) return 0;
    LPNMCUSTOMDRAW cd = (LPNMCUSTOMDRAW)nm;
    switch (cd->dwDrawStage) {
    case CDDS_PREPAINT: {
        RECT hr;
        GetClientRect(cd->hdr.hwndFrom, &hr);
        FillRect(cd->hdc, &hr, g_br_header);
        return CDRF_NOTIFYITEMDRAW;
    }
    case CDDS_ITEMPREPAINT:
        FillRect(cd->hdc, &cd->rc, g_br_header);
        SetBkMode(cd->hdc, TRANSPARENT);
        SetTextColor(cd->hdc, WL_CLR_TEXT);
        SelectObject(cd->hdc, font);
        {
            HDITEMA hi = {0};
            char buf[64];
            hi.mask = HDI_TEXT;
            hi.pszText = buf;
            hi.cchTextMax = sizeof(buf);
            SendMessageA(cd->hdr.hwndFrom, HDM_GETITEMA, cd->dwItemSpec, (LPARAM)&hi);
            RECT tr = cd->rc;
            tr.left += 8;
            DrawTextA(cd->hdc, buf, -1, &tr, DT_VCENTER | DT_SINGLELINE | DT_LEFT);
        }
        return CDRF_SKIPDEFAULT;
    }
    return 0;
}

void wl_track_mouse_leave(HWND hwnd)
{
    TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
    TrackMouseEvent(&tme);
}
