#include "whylag_settings_dlg.h"
#include "whylag_gui_theme.h"
#include "whylag_core.h"
#include <dwmapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

#define IDC_SET_REFRESH   3001
#define IDC_SET_OPENFOLD  3002
#define IDC_SET_OK        3003
#define IDC_SET_CANCEL    3004

static WhyLagSettings *g_edit_settings;
static HWND g_refresh_edit;
static HWND g_openfolder_chk;

static LRESULT CALLBACK settings_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

        CreateWindowExA(0, "STATIC", "Live table refresh (ms):",
            WS_CHILD | WS_VISIBLE, 16, 16, 200, 20, hwnd, NULL, NULL, NULL);
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", g_edit_settings->live_refresh_ms);
        g_refresh_edit = CreateWindowExA(0, "EDIT", buf,
            WS_CHILD | WS_VISIBLE | ES_NUMBER | WS_BORDER,
            16, 40, 120, 24, hwnd, (HMENU)IDC_SET_REFRESH, NULL, NULL);

        g_openfolder_chk = CreateWindowExA(0, "BUTTON", "Open folder after Export CSV",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            16, 78, 280, 22, hwnd, (HMENU)IDC_SET_OPENFOLD, NULL, NULL);
        SendMessage(g_openfolder_chk, BM_SETCHECK,
            g_edit_settings->open_folder_on_export ? BST_CHECKED : BST_UNCHECKED, 0);

        CreateWindowExA(0, "BUTTON", "OK", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            200, 120, 80, 28, hwnd, (HMENU)IDC_SET_OK, NULL, NULL);
        CreateWindowExA(0, "BUTTON", "Cancel", WS_CHILD | WS_VISIBLE,
            288, 120, 80, 28, hwnd, (HMENU)IDC_SET_CANCEL, NULL, NULL);

        SendMessage(g_refresh_edit, WM_SETFONT, (WPARAM)wl_font_ui(), TRUE);
        return 0;
    }
    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wp;
        SetTextColor(hdc, WL_CLR_TEXT);
        SetBkColor(hdc, WL_CLR_EDIT);
        return (LRESULT)wl_brush(3);
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_SET_OK: {
            char buf[16];
            GetWindowTextA(g_refresh_edit, buf, sizeof(buf));
            int ms = atoi(buf);
            if (ms < 500) ms = 500;
            if (ms > 30000) ms = 30000;
            g_edit_settings->live_refresh_ms = ms;
            g_edit_settings->open_folder_on_export =
                (SendMessage(g_openfolder_chk, BM_GETCHECK, 0, 0) == BST_CHECKED);
            whylag_settings_save(g_edit_settings);
            DestroyWindow(hwnd);
            return 0;
        }
        case IDC_SET_CANCEL:
            DestroyWindow(hwnd);
            return 0;
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

void whylag_show_settings_dialog(HWND parent, WhyLagSettings *settings)
{
    if (!settings) return;
    static int registered;
    if (!registered) {
        WNDCLASSEXA wc = {0};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = settings_wndproc;
        wc.hInstance = GetModuleHandleA(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = wl_brush(0);
        wc.lpszClassName = "WhyLagSettingsClass";
        RegisterClassExA(&wc);
        registered = 1;
    }

    g_edit_settings = settings;
    HWND dlg = CreateWindowExA(0, "WhyLagSettingsClass", "whylag Settings",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 400, 190,
        parent, NULL, GetModuleHandleA(NULL), NULL);
    if (dlg) ShowWindow(dlg, SW_SHOW);
}
