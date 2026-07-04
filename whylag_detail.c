#include "whylag_detail.h"
#include "whylag_core.h"
#include "whylag_gui_theme.h"
#include <commctrl.h>
#include <dwmapi.h>
#include <stdio.h>
#include <string.h>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

static const char *g_textdlg_title;
static const char *g_textdlg_body;

static LRESULT CALLBACK textdlg_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        HWND edit = CreateWindowExA(0, "EDIT", g_textdlg_body,
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL,
            12, 12, 556, 360, hwnd, NULL, NULL, NULL);
        SendMessage(edit, WM_SETFONT, (WPARAM)wl_font_ui(), FALSE);
        CreateWindowExA(0, "BUTTON", "Close", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            478, 382, 90, 28, hwnd, (HMENU)1, NULL, NULL);
        return 0;
    }
    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wp;
        SetTextColor(hdc, WL_CLR_TEXT);
        SetBkColor(hdc, WL_CLR_EDIT);
        return (LRESULT)wl_brush(3);
    }
    case WM_COMMAND:
        if (LOWORD(wp) == 1) DestroyWindow(hwnd);
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

void whylag_show_text_dialog(HWND parent, const char *title, const char *body)
{
    if (!body) return;
    static int registered;
    if (!registered) {
        WNDCLASSEXA wc = {0};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = textdlg_wndproc;
        wc.hInstance = GetModuleHandleA(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = "WhyLagTextDlgClass";
        RegisterClassExA(&wc);
        registered = 1;
    }

    g_textdlg_title = title ? title : "whylag";
    g_textdlg_body = body;
    HWND dlg = CreateWindowExA(0, "WhyLagTextDlgClass", g_textdlg_title,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 600, 450,
        parent, NULL, GetModuleHandleA(NULL), NULL);
    if (dlg) ShowWindow(dlg, SW_SHOW);
}

void whylag_show_item_detail(HWND parent, HWND list, int tab_index)
{
    int idx = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    if (idx < 0) idx = ListView_GetNextItem(list, -1, LVNI_FOCUSED);
    if (idx < 0) return;

    char c0[128] = {0}, c1[64] = {0}, c2[64] = {0}, c3[64] = {0}, c4[64] = {0};
    ListView_GetItemText(list, idx, 0, c0, sizeof(c0));
    ListView_GetItemText(list, idx, 1, c1, sizeof(c1));
    ListView_GetItemText(list, idx, 2, c2, sizeof(c2));
    if (tab_index <= 1) ListView_GetItemText(list, idx, 3, c3, sizeof(c3));
    if (tab_index == 2) {
        ListView_GetItemText(list, idx, 3, c3, sizeof(c3));
        ListView_GetItemText(list, idx, 4, c4, sizeof(c4));
    }

    char body[2048];
    char advice[512];

    if (tab_index == 0 || tab_index == 1) {
        WhyLagDriverStats ds;
        int kind = tab_index;
        if (!whylag_lookup_driver(c0, &ds)) {
            snprintf(body, sizeof(body),
                "Driver: %s\r\n\r\nNo stats in memory for this driver.\r\n"
                "Run a sample or reload the last snapshot (saved automatically).\r\n", c0);
        } else {
            whylag_driver_advice(c0, advice, sizeof(advice));
            if (kind == 0) {
                UINT64 avg = ds.dpc_count ? ds.dpc_total_us / ds.dpc_count : 0;
                snprintf(body, sizeof(body),
                    "Driver: %s\r\n\r\n"
                    "DPC count:      %llu\r\n"
                    "DPC max:        %llu us\r\n"
                    "DPC average:    %llu us\r\n"
                    "DPC total time: %llu us\r\n\r\n"
                    "DPCs are deferred interrupt work. High max latency can cause "
                    "audio dropouts, mouse stutter, and UI hitches.\r\n\r\n"
                    "Suggested actions:\r\n%s",
                    c0,
                    (unsigned long long)ds.dpc_count,
                    (unsigned long long)ds.dpc_max_us,
                    (unsigned long long)avg,
                    (unsigned long long)ds.dpc_total_us,
                    advice);
            } else {
                UINT64 avg = ds.isr_count ? ds.isr_total_us / ds.isr_count : 0;
                snprintf(body, sizeof(body),
                    "Driver: %s\r\n\r\n"
                    "ISR count:      %llu\r\n"
                    "ISR max:        %llu us\r\n"
                    "ISR average:    %llu us\r\n"
                    "ISR total time: %llu us\r\n\r\n"
                    "ISRs run at the highest interrupt priority and should stay very short.\r\n\r\n"
                    "Suggested actions:\r\n%s",
                    c0,
                    (unsigned long long)ds.isr_count,
                    (unsigned long long)ds.isr_max_us,
                    (unsigned long long)avg,
                    (unsigned long long)ds.isr_total_us,
                    advice);
            }
        }
        whylag_show_text_dialog(parent, c0, body);
    } else if (tab_index == 2) {
        snprintf(body, sizeof(body),
            "CPU: %s\r\n\r\nDPC events: %s (max %s us)\r\nISR events: %s (max %s us)\r\n\r\n"
            "If one CPU shows much higher max latency, check IRQ affinity and driver load on that core.\r\n",
            c0, c1, c2, c3, c4);
        whylag_show_text_dialog(parent, c0, body);
    } else {
        snprintf(body, sizeof(body),
            "Process: %s\r\nPID: %s\r\nHard page faults: %s\r\n\r\n"
            "Hard faults read memory from disk mid-operation and cause multi-ms stalls.\r\n",
            c0, c1, c2);
        whylag_show_text_dialog(parent, c0, body);
    }
}
