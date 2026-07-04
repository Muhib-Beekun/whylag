/*
 * whylag-gui — Win32 GUI for whylag
 * https://github.com/Muhib-Beekun/whylag
 *
 * MIT License — see LICENSE file.
 */

#define _WIN32_WINNT 0x0602
#include "whylag_core.h"
#include "whylag_gui_theme.h"
#include "whylag_help.h"
#include "whylag_detail.h"
#include "whylag_settings_dlg.h"
#include <commctrl.h>
#include <commdlg.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#ifndef HDM_SETBKCOLOR
#define HDM_SETBKCOLOR (HDM_FIRST + 29)
#endif

#define IDC_START       1001
#define IDC_STOP        1002
#define IDC_DURATION    1003
#define IDC_CONTINUOUS  1004
#define IDC_STATUS      1005
#define IDC_COUNTERS    1006
#define IDC_VERDICT     1007
#define IDC_TAB         1008
#define IDC_LIST_DPC    1009
#define IDC_LIST_ISR    1010
#define IDC_LIST_CPU    1011
#define IDC_LIST_FAULT  1012
#define IDC_EXPORT      1013
#define IDC_COMPARE     1014
#define IDC_ELEVATE     1015
#define IDC_WL_HELP     1026
#define IDC_SETTINGS    1027
#define IDC_PROGRESS    1016
#define IDC_ELAPSED     1017
#define IDC_HEADER      1018
#define IDC_SUBHEADER   1019
#define IDC_LIST_HOST   1020
#define IDC_DUR_LABEL   1021
#define IDC_TAB_DPC     1022
#define IDC_TAB_ISR     1023
#define IDC_TAB_CPU     1024
#define IDC_TAB_FAULT   1025
#define IDI_WHYLAG      101
#define IDT_REFRESH     2001

#define WM_SAMPLE_DONE  (WM_USER + 1)

static HWND g_hwnd;
static HWND g_status, g_counters, g_verdict, g_elapsed, g_subheader, g_list_host;
static HWND g_list_dpc, g_list_isr, g_list_cpu, g_list_fault, g_progress;
static HWND g_duration, g_continuous;
static HWND g_tooltip;
static HWND g_tab_btns[4];
static HANDLE g_sample_thread;
static volatile int g_sampling;
static int g_target_duration;
static int g_continuous_mode;
static int g_active_tab;
static int g_verdict_level;
static double g_last_elapsed;
static char g_last_csv[MAX_PATH];
static int g_btn_hover_id;
static RECT g_tab_strip;
static WhyLagSettings g_settings;
static DWORD g_last_list_refresh;
static int g_content_top_y;

static const char *g_tab_labels[4] = { "DPC drivers", "ISR drivers", "Per-CPU", "Page faults" };

static int is_list_header(HWND hdr)
{
    return hdr == ListView_GetHeader(g_list_dpc) ||
           hdr == ListView_GetHeader(g_list_isr) ||
           hdr == ListView_GetHeader(g_list_cpu) ||
           hdr == ListView_GetHeader(g_list_fault);
}

static WlBtnStyle btn_style_for(int id)
{
    if (id >= IDC_TAB_DPC && id <= IDC_TAB_FAULT)
        return (id - IDC_TAB_DPC == g_active_tab) ? WL_BTN_TAB_ACTIVE : WL_BTN_TAB;
    if (id == IDC_START) return WL_BTN_PRIMARY;
    return WL_BTN_NORMAL;
}

static const char *btn_label_for(int id)
{
    switch (id) {
    case IDC_START: return "Start";
    case IDC_STOP: return "Stop";
    case IDC_EXPORT: return "Export CSV";
    case IDC_COMPARE: return "Compare";
    case IDC_WL_HELP: return "Help";
    case IDC_SETTINGS: return "Opts";
    case IDC_ELEVATE: return "Run as Admin";
    default:
        if (id >= IDC_TAB_DPC && id <= IDC_TAB_FAULT)
            return g_tab_labels[id - IDC_TAB_DPC];
        return "";
    }
}

static void invalidate_buttons(void)
{
    const int ids[] = { IDC_START, IDC_STOP, IDC_EXPORT, IDC_COMPARE, IDC_WL_HELP, IDC_ELEVATE,
                        IDC_TAB_DPC, IDC_TAB_ISR, IDC_TAB_CPU, IDC_TAB_FAULT };
    for (int i = 0; i < (int)(sizeof(ids)/sizeof(ids[0])); i++) {
        HWND b = GetDlgItem(g_hwnd, ids[i]);
        if (b && IsWindowVisible(b)) InvalidateRect(b, NULL, FALSE);
    }
}

static int hit_test_button_client(POINT pt)
{
    const int ids[] = { IDC_START, IDC_STOP, IDC_EXPORT, IDC_COMPARE, IDC_WL_HELP, IDC_ELEVATE,
                        IDC_TAB_DPC, IDC_TAB_ISR, IDC_TAB_CPU, IDC_TAB_FAULT };
    for (int i = 0; i < (int)(sizeof(ids)/sizeof(ids[0])); i++) {
        HWND b = GetDlgItem(g_hwnd, ids[i]);
        if (!b || !IsWindowVisible(b)) continue;
        RECT r;
        GetWindowRect(b, &r);
        MapWindowPoints(NULL, g_hwnd, (POINT *)&r, 2);
        if (PtInRect(&r, pt)) return ids[i];
    }
    return 0;
}

static void update_admin_ui(void)
{
    HWND btn = GetDlgItem(g_hwnd, IDC_ELEVATE);
    if (!btn) return;
    if (whylag_is_admin()) {
        ShowWindow(btn, SW_HIDE);
    } else {
        ShowWindow(btn, SW_SHOW);
        EnableWindow(btn, TRUE);
    }
}

static void apply_font(HWND hwnd, HFONT font)
{
    if (hwnd && font) SendMessage(hwnd, WM_SETFONT, (WPARAM)font, TRUE);
}

static void center_edit_text(HWND edit)
{
    RECT cr;
    GetClientRect(edit, &cr);
    HDC hdc = GetDC(edit);
    HFONT font = (HFONT)SendMessage(edit, WM_GETFONT, 0, 0);
    HFONT oldf = font ? (HFONT)SelectObject(hdc, font) : NULL;
    TEXTMETRIC tm;
    GetTextMetrics(hdc, &tm);
    if (oldf) SelectObject(hdc, oldf);
    ReleaseDC(edit, hdc);

    int pad_y = (cr.bottom - tm.tmHeight) / 2;
    if (pad_y < 1) pad_y = 1;
    RECT tr = cr;
    tr.top = pad_y + 1;
    tr.bottom = cr.bottom - pad_y;
    tr.left = 4;
    tr.right = cr.right - 4;
    SendMessage(edit, EM_SETRECTNP, 0, (LPARAM)&tr);
}

static void add_tooltip(HWND ctl, const char *text)
{
    if (!g_tooltip || !ctl || !text) return;
    TOOLINFOA ti = {0};
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
    ti.hwnd = g_hwnd;
    ti.uId = (UINT_PTR)ctl;
    ti.lpszText = (LPSTR)text;
    SendMessageA(g_tooltip, TTM_ADDTOOLA, 0, (LPARAM)&ti);
}

static void init_tooltips(HWND hwnd)
{
    g_tooltip = CreateWindowExA(0, TOOLTIPS_CLASSA, NULL,
        WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
        0, 0, 0, 0, hwnd, NULL, GetModuleHandleA(NULL), NULL);
    SetWindowTheme(g_tooltip, L"", L"");

    add_tooltip(GetDlgItem(hwnd, IDC_DUR_LABEL), "How long to trace before stopping (seconds)");
    add_tooltip(g_duration, "Sample length in seconds. Ignored when Continuous is checked.");
    add_tooltip(g_continuous, "Trace until you click Stop. Use this to capture a bad period.");
    add_tooltip(GetDlgItem(hwnd, IDC_START), "Start kernel ETW trace (requires Administrator)");
    add_tooltip(GetDlgItem(hwnd, IDC_STOP), "Stop the current trace session");
    add_tooltip(GetDlgItem(hwnd, IDC_EXPORT), "Save results to CSV for baseline vs bad-period comparison");
    add_tooltip(GetDlgItem(hwnd, IDC_COMPARE), "Compare two CSV files and show drivers that regressed");
    add_tooltip(GetDlgItem(hwnd, IDC_SETTINGS), "Settings: live refresh interval, open folder on export");
    add_tooltip(GetDlgItem(hwnd, IDC_WL_HELP), "Open the full help guide (F1)");
    add_tooltip(GetDlgItem(hwnd, IDC_ELEVATE), "Relaunch with Administrator privileges");
    add_tooltip(g_elapsed, "Current sample progress");
    add_tooltip(g_counters, "Live event counts during sampling");
    add_tooltip(g_verdict, "Overall latency verdict based on worst DPC and ISR in the sample");
    add_tooltip(g_tab_btns[0], "Drivers ranked by DPC latency - deferred interrupt work");
    add_tooltip(g_tab_btns[1], "Drivers ranked by ISR latency - immediate interrupt handlers");
    add_tooltip(g_tab_btns[2], "Per-logical-CPU DPC and ISR counts and worst latency");
    add_tooltip(g_tab_btns[3], "Processes causing hard page faults (disk stalls)");
    add_tooltip(g_list_dpc, "Double-click a driver for DPC detail and fix suggestions");
    add_tooltip(g_list_isr, "Double-click a driver for ISR detail and fix suggestions");
    add_tooltip(g_list_cpu, "Double-click a CPU row for per-core summary");
    add_tooltip(g_list_fault, "Double-click a process for page-fault detail");
}

static LRESULT CALLBACK elapsed_subclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
    UINT_PTR id, DWORD_PTR ref)
{
    (void)id; (void)ref;
    if (msg == WM_ERASEBKGND)
        return 1;
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        wl_draw_static_text(hdc, hwnd, wl_font_ui(), WL_CLR_TEXT, wl_brush(0));
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK verdict_subclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
    UINT_PTR id, DWORD_PTR ref)
{
    (void)id; (void)ref;
    if (msg == WM_ERASEBKGND)
        return 1;
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT r;
        GetClientRect(hwnd, &r);
        FillRect(hdc, &r, wl_brush(1));
        if (g_verdict_level > 0) {
            RECT bar = r;
            bar.right = bar.left + 3;
            HBRUSH b = CreateSolidBrush(g_verdict_level == 2 ? WL_CLR_BAD : WL_CLR_WARN);
            FillRect(hdc, &bar, b);
            DeleteObject(b);
        }
        char buf[512];
        GetWindowTextA(hwnd, buf, sizeof(buf));
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, WL_CLR_TEXT);
        SelectObject(hdc, wl_font_ui());
        DrawTextA(hdc, buf, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static void set_status(const char *text) { SetWindowTextA(g_status, text); }

static void style_list(HWND list)
{
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    ListView_SetBkColor(list, WL_CLR_PANEL);
    ListView_SetTextBkColor(list, WL_CLR_PANEL);
    ListView_SetTextColor(list, WL_CLR_TEXT);
    HWND hdr = ListView_GetHeader(list);
    if (hdr) {
        SetWindowTheme(hdr, L"", L"");
        SendMessage(hdr, HDM_SETBKCOLOR, 0, (LPARAM)WL_CLR_HEADER);
    }
}

static LRESULT CALLBACK list_host_subclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
    UINT_PTR id, DWORD_PTR ref)
{
    (void)id; (void)ref;
    if (msg == WM_ERASEBKGND) {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wp, &rc, wl_brush(1));
        return 1;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK list_subclass(HWND lv, UINT msg, WPARAM wp, LPARAM lp,
    UINT_PTR id, DWORD_PTR ref)
{
    (void)id; (void)ref;
    if (msg == WM_NOTIFY) {
        NMHDR *nm = (NMHDR *)lp;
        if (is_list_header(nm->hwndFrom)) {
            LRESULT r = wl_header_custom_draw(nm, wl_font_ui());
            if (r) return r;
        }
    }
    return DefSubclassProc(lv, msg, wp, lp);
}

static void stretch_list_columns(HWND list)
{
    HWND hdr = ListView_GetHeader(list);
    if (!hdr) return;
    int cols = (int)SendMessageA(hdr, HDM_GETITEMCOUNT, 0, 0);
    if (cols <= 0) return;

    RECT rc;
    GetClientRect(list, &rc);
    int total = rc.right;
    if (GetWindowLong(list, GWL_STYLE) & WS_VSCROLL)
        total -= GetSystemMetrics(SM_CXVSCROLL);

    int used = 0;
    for (int i = 0; i < cols - 1; i++)
        used += ListView_GetColumnWidth(list, i);
    int last_w = total - used;
    if (last_w > 40)
        ListView_SetColumnWidth(list, cols - 1, last_w);
}

static void init_list_columns(HWND list, const char *kind)
{
    style_list(list);
    LVCOLUMNA col = {0};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    if (strcmp(kind, "driver") == 0) {
        col.pszText = "Driver"; col.cx = 220; ListView_InsertColumn(list, 0, &col);
        col.pszText = "Count"; col.cx = 80; ListView_InsertColumn(list, 1, &col);
        col.pszText = "Max (us)"; col.cx = 90; ListView_InsertColumn(list, 2, &col);
        col.pszText = "Avg (us)"; col.cx = 90; ListView_InsertColumn(list, 3, &col);
    } else if (strcmp(kind, "cpu") == 0) {
        col.pszText = "CPU"; col.cx = 70; ListView_InsertColumn(list, 0, &col);
        col.pszText = "DPC #"; col.cx = 80; ListView_InsertColumn(list, 1, &col);
        col.pszText = "DPC max"; col.cx = 90; ListView_InsertColumn(list, 2, &col);
        col.pszText = "ISR #"; col.cx = 80; ListView_InsertColumn(list, 3, &col);
        col.pszText = "ISR max"; col.cx = 90; ListView_InsertColumn(list, 4, &col);
    } else {
        col.pszText = "Process"; col.cx = 220; ListView_InsertColumn(list, 0, &col);
        col.pszText = "PID"; col.cx = 80; ListView_InsertColumn(list, 1, &col);
        col.pszText = "Faults"; col.cx = 90; ListView_InsertColumn(list, 2, &col);
    }
    stretch_list_columns(list);
}

static void list_add_row(HWND list, const char *c0, const char *c1, const char *c2, const char *c3, const char *c4)
{
    LVITEMA item = {0};
    item.mask = LVIF_TEXT;
    item.iItem = ListView_GetItemCount(list);
    item.pszText = (char *)c0;
    int idx = ListView_InsertItem(list, &item);
    ListView_SetItemText(list, idx, 1, (char *)c1);
    ListView_SetItemText(list, idx, 2, (char *)c2);
    if (c3) ListView_SetItemText(list, idx, 3, (char *)c3);
    if (c4) ListView_SetItemText(list, idx, 4, (char *)c4);
}

static int cmp_dpc_desc(const void *a, const void *b)
{
    const WhyLagDriverStats *sa = (const WhyLagDriverStats *)a;
    const WhyLagDriverStats *sb = (const WhyLagDriverStats *)b;
    if (sb->dpc_max_us > sa->dpc_max_us) return 1;
    if (sb->dpc_max_us < sa->dpc_max_us) return -1;
    return 0;
}

static int cmp_isr_desc(const void *a, const void *b)
{
    const WhyLagDriverStats *sa = (const WhyLagDriverStats *)a;
    const WhyLagDriverStats *sb = (const WhyLagDriverStats *)b;
    if (sb->isr_max_us > sa->isr_max_us) return 1;
    if (sb->isr_max_us < sa->isr_max_us) return -1;
    return 0;
}

static void format_verdict(char *buf, size_t buflen, int *level_out)
{
    WhyLagDriverStats stats[512];
    int count = 0;
    whylag_copy_driver_stats(stats, 512, &count);

    UINT64 worst_dpc = 0, worst_isr = 0;
    const char *dpc_drv = "(none)", *isr_drv = "(none)";
    for (int i = 0; i < count; i++) {
        if (stats[i].dpc_max_us > worst_dpc) { worst_dpc = stats[i].dpc_max_us; dpc_drv = stats[i].name; }
        if (stats[i].isr_max_us > worst_isr) { worst_isr = stats[i].isr_max_us; isr_drv = stats[i].name; }
    }

    int level = 0;
    const char *label = "OK";
    if (worst_dpc >= 5000 || worst_isr >= 2000) { level = 2; label = "BAD"; }
    else if (worst_dpc >= 1000 || worst_isr >= 500) { level = 1; label = "WARN"; }
    if (level_out) *level_out = level;

    snprintf(buf, buflen,
             "%s  |  Worst DPC: %llu us (%s)  |  Worst ISR: %llu us (%s)",
             label,
             (unsigned long long)worst_dpc, dpc_drv,
             (unsigned long long)worst_isr, isr_drv);
}

static void refresh_lists(void)
{
    WhyLagDriverStats stats[512];
    WhyLagCpuStats cpus[WHYLAG_MAX_CPUS];
    WhyLagProcFaults faults[256];
    int sc = 0, cc = 0, fc = 0;
    char buf[64];

    whylag_copy_driver_stats(stats, 512, &sc);
    whylag_copy_cpu_stats(cpus, WHYLAG_MAX_CPUS, &cc);
    whylag_copy_fault_stats(faults, 256, &fc);

    ListView_DeleteAllItems(g_list_dpc);
    ListView_DeleteAllItems(g_list_isr);
    ListView_DeleteAllItems(g_list_cpu);
    ListView_DeleteAllItems(g_list_fault);

    qsort(stats, sc, sizeof(WhyLagDriverStats), cmp_dpc_desc);
    int shown = 0;
    for (int i = 0; i < sc && shown < 25; i++) {
        if (stats[i].dpc_count == 0) continue;
        snprintf(buf, sizeof(buf), "%llu", (unsigned long long)stats[i].dpc_count);
        char maxb[32], avgb[32];
        snprintf(maxb, sizeof(maxb), "%llu", (unsigned long long)stats[i].dpc_max_us);
        snprintf(avgb, sizeof(avgb), "%llu", (unsigned long long)(stats[i].dpc_total_us / stats[i].dpc_count));
        list_add_row(g_list_dpc, stats[i].name, buf, maxb, avgb, NULL);
        shown++;
    }

    qsort(stats, sc, sizeof(WhyLagDriverStats), cmp_isr_desc);
    shown = 0;
    for (int i = 0; i < sc && shown < 25; i++) {
        if (stats[i].isr_count == 0) continue;
        snprintf(buf, sizeof(buf), "%llu", (unsigned long long)stats[i].isr_count);
        char maxb[32], avgb[32];
        snprintf(maxb, sizeof(maxb), "%llu", (unsigned long long)stats[i].isr_max_us);
        snprintf(avgb, sizeof(avgb), "%llu", (unsigned long long)(stats[i].isr_total_us / stats[i].isr_count));
        list_add_row(g_list_isr, stats[i].name, buf, maxb, avgb, NULL);
        shown++;
    }

    for (int i = 0; i < cc; i++) {
        if (cpus[i].dpc_count == 0 && cpus[i].isr_count == 0) continue;
        char label[32], dpcn[32], dpcm[32], isrn[32], isrm[32];
        snprintf(label, sizeof(label), "CPU %d", i);
        snprintf(dpcn, sizeof(dpcn), "%llu", (unsigned long long)cpus[i].dpc_count);
        snprintf(dpcm, sizeof(dpcm), "%llu", (unsigned long long)cpus[i].dpc_max_us);
        snprintf(isrn, sizeof(isrn), "%llu", (unsigned long long)cpus[i].isr_count);
        snprintf(isrm, sizeof(isrm), "%llu", (unsigned long long)cpus[i].isr_max_us);
        list_add_row(g_list_cpu, label, dpcn, dpcm, isrn, isrm);
    }

    for (int i = 0; i < fc; i++) {
        if (faults[i].fault_count == 0) continue;
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)faults[i].pid);
        char fb[32];
        snprintf(fb, sizeof(fb), "%llu", (unsigned long long)faults[i].fault_count);
        list_add_row(g_list_fault, faults[i].name, buf, fb, NULL, NULL);
    }

    char vbuf[512];
    format_verdict(vbuf, sizeof(vbuf), &g_verdict_level);
    SetWindowTextA(g_verdict, vbuf);
    InvalidateRect(g_verdict, NULL, FALSE);
}

static void update_live_ui(void)
{
    UINT64 total, dpc, isr, faults, cswitch, disk;
    whylag_get_event_counts(&total, &dpc, &isr, &faults);
    whylag_get_extra_counts(&cswitch, &disk);
    char buf[400];
    snprintf(buf, sizeof(buf),
             "Events: %llu   DPC: %llu   ISR: %llu   Faults: %llu   CSwitch: %llu   Disk: %llu   Modules: %d",
             (unsigned long long)total, (unsigned long long)dpc,
             (unsigned long long)isr, (unsigned long long)faults,
             (unsigned long long)cswitch, (unsigned long long)disk,
             whylag_get_module_count());
    SetWindowTextA(g_counters, buf);

    if (g_sampling) {
        DWORD now = GetTickCount();
        if (now - g_last_list_refresh >= (DWORD)g_settings.live_refresh_ms) {
            refresh_lists();
            g_last_list_refresh = now;
        }
    }

    int elapsed = 0, active = 0;
    whylag_get_sample_progress(&elapsed, &active);

    if (g_sampling && active) {
        char ebuf[128];
        if (g_continuous_mode) {
            snprintf(ebuf, sizeof(ebuf), "Recording... %d sec (continuous)", elapsed);
            SendMessage(g_progress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
            SendMessage(g_progress, PBM_SETPOS, (elapsed % 100), 0);
        } else {
            snprintf(ebuf, sizeof(ebuf), "Recording... %d / %d sec", elapsed, g_target_duration);
            int pct = g_target_duration > 0 ? (elapsed * 100 / g_target_duration) : 0;
            if (pct > 100) pct = 100;
            SendMessage(g_progress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
            SendMessage(g_progress, PBM_SETPOS, pct, 0);
        }
        SetWindowTextA(g_elapsed, ebuf);
        InvalidateRect(g_elapsed, NULL, FALSE);
    }
}

typedef struct { WhyLagOptions opts; } SampleParams;

static DWORD WINAPI sample_thread(LPVOID param)
{
    SampleParams *sp = (SampleParams *)param;
    double elapsed = 0;

    if (whylag_start_session(1) != 0) {
        PostMessage(g_hwnd, WM_SAMPLE_DONE, 1, 0);
        free(sp);
        return 1;
    }

    whylag_run_loop(&sp->opts, &elapsed);
    whylag_request_stop();
    whylag_stop_session();
    Sleep(500);

    g_last_elapsed = elapsed;
    PostMessage(g_hwnd, WM_SAMPLE_DONE, 0, 0);
    free(sp);
    return 0;
}

static void relaunch_elevated(void)
{
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    HINSTANCE r = ShellExecuteA(g_hwnd, "runas", path, NULL, NULL, SW_SHOW);
    if ((INT_PTR)r > 32)
        DestroyWindow(g_hwnd);
}

static void start_sampling(void)
{
    if (g_sampling) return;

    if (!whylag_is_admin()) {
        int r = MessageBoxA(g_hwnd,
            "Kernel tracing requires Administrator privileges.\n\nRelaunch elevated now?",
            "whylag", MB_YESNO | MB_ICONWARNING);
        if (r == IDYES) relaunch_elevated();
        return;
    }

    char durbuf[16];
    GetWindowTextA(g_duration, durbuf, sizeof(durbuf));
    g_target_duration = atoi(durbuf);
    if (g_target_duration < 1) g_target_duration = 10;
    g_settings.duration = g_target_duration;
    whylag_settings_save(&g_settings);

    SampleParams *sp = (SampleParams *)calloc(1, sizeof(SampleParams));
    sp->opts.duration = g_target_duration;
    sp->opts.interval = 5;
    sp->opts.quiet = 1;
    g_continuous_mode = (SendMessage(g_continuous, BM_GETCHECK, 0, 0) == BST_CHECKED);
    sp->opts.continuous = g_continuous_mode;

    g_running = 1;
    g_sampling = 1;
    g_last_list_refresh = GetTickCount();
    EnableWindow(GetDlgItem(g_hwnd, IDC_START), FALSE);
    EnableWindow(GetDlgItem(g_hwnd, IDC_STOP), TRUE);
    EnableWindow(g_duration, FALSE);
    EnableWindow(g_continuous, FALSE);
    SendMessage(g_progress, PBM_SETPOS, 0, 0);
    set_status("Trace session active - collecting kernel latency events...");

    g_sample_thread = CreateThread(NULL, 0, sample_thread, sp, 0, NULL);
}

static void stop_sampling(void)
{
    if (!g_sampling) return;
    whylag_request_stop();
    set_status("Stopping trace session...");
}

static void on_sample_done(WPARAM failed)
{
    if (g_sample_thread) {
        WaitForSingleObject(g_sample_thread, 30000);
        CloseHandle(g_sample_thread);
        g_sample_thread = NULL;
    }

    g_sampling = 0;
    EnableWindow(GetDlgItem(g_hwnd, IDC_START), TRUE);
    EnableWindow(GetDlgItem(g_hwnd, IDC_STOP), FALSE);
    EnableWindow(g_duration, TRUE);
    EnableWindow(g_continuous, TRUE);
    SendMessage(g_progress, PBM_SETPOS, failed ? 0 : 100, 0);

    if (failed) {
        SetWindowTextA(g_elapsed, "Failed - not elevated");
        set_status("Could not start ETW trace. Run as Administrator.");
        return;
    }

    refresh_lists();
    update_live_ui();
    whylag_save_snapshot(g_last_elapsed);
    g_settings.last_sample_elapsed_sec = (int)(g_last_elapsed + 0.5);
    whylag_settings_save(&g_settings);
    char ebuf[128];
    snprintf(ebuf, sizeof(ebuf), "Complete - %.1f sec sampled", g_last_elapsed);
    SetWindowTextA(g_elapsed, ebuf);
    InvalidateRect(g_elapsed, NULL, FALSE);
    set_status("Sample complete. Review drivers with highest Max (us). Export CSV to compare baseline vs bad periods.");
}

static void export_csv_dialog(void)
{
    char path[MAX_PATH] = "whylag-report.csv";
    if (g_settings.last_export_dir[0]) {
        snprintf(path, sizeof(path), "%s\\whylag-report.csv", g_settings.last_export_dir);
    }
    OPENFILENAMEA ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = "CSV files\0*.csv\0All files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT;
    ofn.lpstrDefExt = "csv";
    if (!GetSaveFileNameA(&ofn)) return;
    if (whylag_export_csv(g_last_elapsed > 0 ? g_last_elapsed : 10.0, path) == 0) {
        strncpy(g_last_csv, path, MAX_PATH - 1);
        char *slash = strrchr(path, '\\');
        if (slash) {
            *slash = 0;
            strncpy(g_settings.last_export_dir, path, MAX_PATH - 1);
            whylag_settings_save(&g_settings);
        }
        char msg[512];
        snprintf(msg, sizeof(msg), "Exported: %s", g_last_csv);
        set_status(msg);
        if (g_settings.open_folder_on_export)
            whylag_open_folder_for_file(g_last_csv);
    }
}

static void compare_csv_dialog(void)
{
    char path1[MAX_PATH] = {0}, path2[MAX_PATH] = {0};
    OPENFILENAMEA ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = "CSV files\0*.csv\0All files\0*.*\0";
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST;

    ofn.lpstrFile = path1;
    ofn.lpstrTitle = "Baseline CSV (system felt fine)";
    if (!GetOpenFileNameA(&ofn)) return;

    ofn.lpstrFile = path2;
    ofn.lpstrTitle = "Bad-period CSV (while stuttering)";
    if (!GetOpenFileNameA(&ofn)) return;

    char report[8192];
    if (whylag_compare_csv(path1, path2, report, sizeof(report)) != 0) {
        MessageBoxA(g_hwnd, "Could not read one or both CSV files.", "Compare", MB_OK | MB_ICONERROR);
        return;
    }
    whylag_show_text_dialog(g_hwnd, "Baseline vs bad-period", report);
}

static void switch_tab(int index)
{
    g_active_tab = index;
    ShowWindow(g_list_dpc, index == 0 ? SW_SHOW : SW_HIDE);
    ShowWindow(g_list_isr, index == 1 ? SW_SHOW : SW_HIDE);
    ShowWindow(g_list_cpu, index == 2 ? SW_SHOW : SW_HIDE);
    ShowWindow(g_list_fault, index == 3 ? SW_SHOW : SW_HIDE);
    SetFocus(g_hwnd);
    invalidate_buttons();
}

static void layout_controls(int w, int h)
{
    int pad = 20;
    int y = pad;
    int row_h = 32;

    MoveWindow(g_subheader, pad, y, w - pad * 2, 20, TRUE);
    y += 26;

    MoveWindow(GetDlgItem(g_hwnd, IDC_DUR_LABEL), pad, y, 58, row_h, TRUE);
    MoveWindow(g_duration, pad + 62, y, 52, row_h, TRUE);
    center_edit_text(g_duration);
    MoveWindow(g_continuous, pad + 124, y, 110, row_h, TRUE);
    MoveWindow(GetDlgItem(g_hwnd, IDC_START), pad + 250, y, 88, row_h, TRUE);
    MoveWindow(GetDlgItem(g_hwnd, IDC_STOP), pad + 346, y, 88, row_h, TRUE);
    MoveWindow(GetDlgItem(g_hwnd, IDC_EXPORT), pad + 446, y, 96, row_h, TRUE);
    MoveWindow(GetDlgItem(g_hwnd, IDC_COMPARE), pad + 552, y, 96, row_h, TRUE);
    MoveWindow(GetDlgItem(g_hwnd, IDC_SETTINGS), pad + 654, y, 52, row_h, TRUE);
    MoveWindow(GetDlgItem(g_hwnd, IDC_WL_HELP), pad + 712, y, 72, row_h, TRUE);
    if (IsWindowVisible(GetDlgItem(g_hwnd, IDC_ELEVATE)))
        MoveWindow(GetDlgItem(g_hwnd, IDC_ELEVATE), w - pad - 120, y, 120, row_h, TRUE);
    y += row_h + 12;

    MoveWindow(g_elapsed, pad, y, w - pad * 2, 20, TRUE);
    y += 24;
    MoveWindow(g_progress, pad, y, w - pad * 2, 10, TRUE);
    y += 18;
    MoveWindow(g_counters, pad, y, w - pad * 2, 22, TRUE);
    y += 30;
    MoveWindow(g_verdict, pad, y, w - pad * 2, 32, TRUE);
    y += 44;

    int footer_h = 28;
    int tab_h = 34;
    int total_tab_w = w - pad * 2;
    g_content_top_y = y + tab_h;

    SetRect(&g_tab_strip, pad, y, w - pad, g_content_top_y);

    for (int i = 0; i < 4; i++) {
        int x0 = pad + (total_tab_w * i) / 4;
        int x1 = pad + (total_tab_w * (i + 1)) / 4;
        MoveWindow(g_tab_btns[i], x0, y, x1 - x0, tab_h, TRUE);
    }

    MoveWindow(g_list_host, pad, g_content_top_y, total_tab_w, h - g_content_top_y - footer_h - pad, TRUE);
    {
        RECT hostrc;
        GetClientRect(g_list_host, &hostrc);
        MoveWindow(g_list_dpc, 0, 0, hostrc.right, hostrc.bottom, TRUE);
        MoveWindow(g_list_isr, 0, 0, hostrc.right, hostrc.bottom, TRUE);
        MoveWindow(g_list_cpu, 0, 0, hostrc.right, hostrc.bottom, TRUE);
        MoveWindow(g_list_fault, 0, 0, hostrc.right, hostrc.bottom, TRUE);
        stretch_list_columns(g_list_dpc);
        stretch_list_columns(g_list_isr);
        stretch_list_columns(g_list_cpu);
        stretch_list_columns(g_list_fault);
    }
    MoveWindow(g_status, pad, h - footer_h - 4, w - pad * 2, footer_h, TRUE);
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        g_hwnd = hwnd;
        g_active_tab = 0;
        wl_theme_init();

        BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

        g_subheader = CreateWindowExA(0, "STATIC", "Kernel latency diagnostics for Windows",
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE, 0, 0, 100, 20, hwnd, (HMENU)IDC_SUBHEADER, NULL, NULL);
        apply_font(g_subheader, wl_font_ui());

        CreateWindowExA(0, "STATIC", "Duration",
            WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, 0, 0, 58, 32, hwnd, (HMENU)IDC_DUR_LABEL, NULL, NULL);
        g_duration = CreateWindowExA(0, "EDIT", "10",
            WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_CENTER | WS_BORDER,
            0, 0, 52, 32, hwnd, (HMENU)IDC_DURATION, NULL, NULL);
        g_continuous = CreateWindowExA(0, "BUTTON", "Continuous",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | BS_AUTOCHECKBOX,
            0, 0, 110, 32, hwnd, (HMENU)IDC_CONTINUOUS, NULL, NULL);

        CreateWindowExA(0, "BUTTON", "Start", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            0, 0, 88, 32, hwnd, (HMENU)IDC_START, NULL, NULL);
        CreateWindowExA(0, "BUTTON", "Stop", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            0, 0, 88, 32, hwnd, (HMENU)IDC_STOP, NULL, NULL);
        EnableWindow(GetDlgItem(hwnd, IDC_STOP), FALSE);
        CreateWindowExA(0, "BUTTON", "Export CSV", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            0, 0, 96, 32, hwnd, (HMENU)IDC_EXPORT, NULL, NULL);
        CreateWindowExA(0, "BUTTON", "Compare", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            0, 0, 96, 32, hwnd, (HMENU)IDC_COMPARE, NULL, NULL);
        CreateWindowExA(0, "BUTTON", "Opts", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            0, 0, 52, 32, hwnd, (HMENU)IDC_SETTINGS, NULL, NULL);
        CreateWindowExA(0, "BUTTON", "Help", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            0, 0, 72, 32, hwnd, (HMENU)IDC_WL_HELP, NULL, NULL);
        CreateWindowExA(0, "BUTTON", "Run as Admin", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            0, 0, 120, 32, hwnd, (HMENU)IDC_ELEVATE, NULL, NULL);

        g_elapsed = CreateWindowExA(0, "STATIC", "Ready to sample",
            WS_CHILD | WS_VISIBLE, 0, 0, 100, 20, hwnd, (HMENU)IDC_ELAPSED, NULL, NULL);
        g_progress = CreateWindowExA(0, PROGRESS_CLASSA, NULL,
            WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
            0, 0, 100, 10, hwnd, (HMENU)IDC_PROGRESS, NULL, NULL);
        SendMessage(g_progress, PBM_SETBARCOLOR, 0, WL_CLR_ACCENT);
        SendMessage(g_progress, PBM_SETBKCOLOR, 0, WL_CLR_BORDER);

        g_counters = CreateWindowExA(0, "STATIC", "Events: 0   DPC: 0   ISR: 0   Faults: 0   CSwitch: 0   Disk: 0   Modules: 0",
            WS_CHILD | WS_VISIBLE, 0, 0, 100, 22, hwnd, (HMENU)IDC_COUNTERS, NULL, NULL);
        g_verdict = CreateWindowExA(0, "STATIC", "Run a sample to see verdict",
            WS_CHILD | WS_VISIBLE | SS_CENTER, 0, 0, 100, 32, hwnd, (HMENU)IDC_VERDICT, NULL, NULL);
        g_status = CreateWindowExA(0, "STATIC", "Baseline when fine, capture when stuttering, then Compare CSVs.",
            WS_CHILD | WS_VISIBLE, 0, 0, 100, 20, hwnd, (HMENU)IDC_STATUS, NULL, NULL);

        for (int i = 0; i < 4; i++) {
            g_tab_btns[i] = CreateWindowExA(0, "BUTTON", g_tab_labels[i],
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                0, 0, 100, 34, hwnd, (HMENU)(INT_PTR)(IDC_TAB_DPC + i), NULL, NULL);
        }

        g_list_host = CreateWindowExA(0, "STATIC", NULL,
            WS_CHILD | WS_VISIBLE, 0, 0, 100, 100, hwnd, (HMENU)IDC_LIST_HOST, NULL, NULL);

        g_list_dpc = CreateWindowExA(0, WC_LISTVIEWA, "",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            0, 0, 100, 100, g_list_host, (HMENU)IDC_LIST_DPC, NULL, NULL);
        g_list_isr = CreateWindowExA(0, WC_LISTVIEWA, "",
            WS_CHILD | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            0, 0, 100, 100, g_list_host, (HMENU)IDC_LIST_ISR, NULL, NULL);
        g_list_cpu = CreateWindowExA(0, WC_LISTVIEWA, "",
            WS_CHILD | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            0, 0, 100, 100, g_list_host, (HMENU)IDC_LIST_CPU, NULL, NULL);
        g_list_fault = CreateWindowExA(0, WC_LISTVIEWA, "",
            WS_CHILD | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            0, 0, 100, 100, g_list_host, (HMENU)IDC_LIST_FAULT, NULL, NULL);

        init_list_columns(g_list_dpc, "driver");
        init_list_columns(g_list_isr, "driver");
        init_list_columns(g_list_cpu, "cpu");
        init_list_columns(g_list_fault, "fault");
        SetWindowSubclass(g_list_host, list_host_subclass, 1, 0);
        SetWindowSubclass(g_list_dpc, list_subclass, 2, 0);
        SetWindowSubclass(g_list_isr, list_subclass, 3, 0);
        SetWindowSubclass(g_list_cpu, list_subclass, 4, 0);
        SetWindowSubclass(g_list_fault, list_subclass, 5, 0);
        SetWindowSubclass(g_verdict, verdict_subclass, 6, 0);
        SetWindowSubclass(g_elapsed, elapsed_subclass, 7, 0);
        switch_tab(0);

        apply_font(GetDlgItem(hwnd, IDC_DUR_LABEL), wl_font_ui());
        apply_font(g_duration, wl_font_ui());
        center_edit_text(g_duration);
        apply_font(g_continuous, wl_font_ui());
        apply_font(g_elapsed, wl_font_ui());
        apply_font(g_status, wl_font_ui());
        apply_font(g_verdict, wl_font_ui());
        apply_font(g_counters, wl_font_mono());

        update_admin_ui();

        whylag_settings_load(&g_settings);
        char dur_init[16];
        snprintf(dur_init, sizeof(dur_init), "%d", g_settings.duration > 0 ? g_settings.duration : 10);
        SetWindowTextA(g_duration, dur_init);

        char scheck[320];
        whylag_self_check(scheck, sizeof(scheck));
        if (!whylag_is_admin())
            set_status(scheck);
        else if (scheck[0])
            set_status(scheck);
        else
            set_status("Ready. Run a baseline sample when the system feels fine.");

        if (whylag_load_snapshot(&g_last_elapsed) == 0) {
            refresh_lists();
            update_live_ui();
            char ebuf[128];
            snprintf(ebuf, sizeof(ebuf), "Last sample: %.1f sec (restored)", g_last_elapsed);
            SetWindowTextA(g_elapsed, ebuf);
            InvalidateRect(g_elapsed, NULL, FALSE);
        } else if (g_settings.last_sample_elapsed_sec > 0) {
            g_last_elapsed = g_settings.last_sample_elapsed_sec;
        }

        RECT rc;
        GetClientRect(hwnd, &rc);
        layout_controls(rc.right, rc.bottom);
        init_tooltips(hwnd);

        SetTimer(hwnd, IDT_REFRESH, 400, NULL);
        return 0;
    }
    case WM_SIZE: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        layout_controls(rc.right, rc.bottom);
        return 0;
    }
    case WM_MOUSEMOVE: {
        wl_track_mouse_leave(hwnd);
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int hit = hit_test_button_client(pt);
        if (hit != g_btn_hover_id) {
            int old = g_btn_hover_id;
            g_btn_hover_id = hit;
            if (old) {
                HWND b = GetDlgItem(hwnd, old);
                if (b) InvalidateRect(b, NULL, FALSE);
            }
            if (hit) {
                HWND b = GetDlgItem(hwnd, hit);
                if (b) InvalidateRect(b, NULL, FALSE);
            }
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_btn_hover_id) {
            HWND b = GetDlgItem(g_hwnd, g_btn_hover_id);
            g_btn_hover_id = 0;
            if (b) InvalidateRect(b, NULL, FALSE);
        }
        return 0;
    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT dis = (LPDRAWITEMSTRUCT)lp;
        if (dis->CtlType == ODT_BUTTON) {
            if (dis->CtlID == IDC_CONTINUOUS) {
                wl_draw_checkbox(dis, "Continuous", wl_font_ui());
                return TRUE;
            }
            wl_draw_button(dis, btn_style_for((int)dis->CtlID), btn_label_for((int)dis->CtlID), wl_font_ui());
            return TRUE;
        }
        break;
    }
    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wp;
        HWND ctl = (HWND)lp;
        SetBkMode(hdc, TRANSPARENT);
        if (ctl == g_verdict)
            return (LRESULT)wl_brush(1);
        if (ctl == g_subheader) {
            SetTextColor(hdc, WL_CLR_MUTED);
            return (LRESULT)wl_brush(0);
        }
        if (ctl == g_list_host) {
            SetBkColor(hdc, WL_CLR_PANEL);
            return (LRESULT)wl_brush(1);
        }
        SetTextColor(hdc, (ctl == g_status) ? WL_CLR_MUTED : WL_CLR_TEXT);
        return (LRESULT)wl_brush(0);
    }
    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wp;
        SetTextColor(hdc, WL_CLR_TEXT);
        SetBkColor(hdc, WL_CLR_EDIT);
        return (LRESULT)wl_brush(3);
    }
    case WM_CTLCOLORBTN:
        return (LRESULT)GetStockObject(NULL_BRUSH);
    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wp;
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, wl_brush(0));
        if (g_tab_strip.bottom > g_tab_strip.top)
            FillRect(hdc, &g_tab_strip, wl_brush(0));
        return 1;
    }
    case WM_KEYDOWN:
        if (wp == VK_F1) {
            whylag_show_help(hwnd);
            return 0;
        }
        break;
    case WM_NOTIFY: {
        NMHDR *nm = (NMHDR *)lp;
        if (nm->code == NM_DBLCLK) {
            if (nm->hwndFrom == g_list_dpc)
                whylag_show_item_detail(hwnd, g_list_dpc, 0);
            else if (nm->hwndFrom == g_list_isr)
                whylag_show_item_detail(hwnd, g_list_isr, 1);
            else if (nm->hwndFrom == g_list_cpu)
                whylag_show_item_detail(hwnd, g_list_cpu, 2);
            else if (nm->hwndFrom == g_list_fault)
                whylag_show_item_detail(hwnd, g_list_fault, 3);
            return 0;
        }
        if (is_list_header(nm->hwndFrom)) {
            LRESULT r = wl_header_custom_draw(nm, wl_font_ui());
            if (r) return r;
        }
        return 0;
    }
    case WM_TIMER:
        if (wp == IDT_REFRESH) update_live_ui();
        return 0;
    case WM_SAMPLE_DONE:
        on_sample_done(wp);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_START: start_sampling(); break;
        case IDC_STOP:  stop_sampling(); break;
        case IDC_EXPORT: export_csv_dialog(); break;
        case IDC_COMPARE: compare_csv_dialog(); break;
        case IDC_SETTINGS:
            whylag_show_settings_dialog(hwnd, &g_settings);
            whylag_settings_save(&g_settings);
            break;
        case IDC_WL_HELP: whylag_show_help(hwnd); break;
        case IDC_ELEVATE: relaunch_elevated(); break;
        case IDC_TAB_DPC: switch_tab(0); break;
        case IDC_TAB_ISR: switch_tab(1); break;
        case IDC_TAB_CPU: switch_tab(2); break;
        case IDC_TAB_FAULT: switch_tab(3); break;
        case IDC_CONTINUOUS:
            InvalidateRect(g_continuous, NULL, FALSE);
            break;
        }
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, IDT_REFRESH);
        whylag_settings_save(&g_settings);
        whylag_request_stop();
        if (g_sample_thread) { WaitForSingleObject(g_sample_thread, 5000); CloseHandle(g_sample_thread); }
        wl_theme_shutdown();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

static void enable_dpi_awareness(void)
{
    typedef BOOL (WINAPI *Fn)(HANDLE);
    HMODULE u32 = GetModuleHandleA("user32.dll");
    Fn fn = u32 ? (Fn)GetProcAddress(u32, "SetProcessDpiAwarenessContext") : NULL;
    if (fn) fn((HANDLE)(INT_PTR)-4); /* PER_MONITOR_AWARE_V2 */
    else SetProcessDPIAware();
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmdLine, int show)
{
    (void)hPrev; (void)cmdLine;

    enable_dpi_awareness();

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&icc);

    WNDCLASSEXA wc = {0};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIcon(hInst, MAKEINTRESOURCE(IDI_WHYLAG));
    wc.hIconSm = LoadIcon(hInst, MAKEINTRESOURCE(IDI_WHYLAG));
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = "WhyLagGuiClass";
    RegisterClassExA(&wc);

    char title[64];
    snprintf(title, sizeof(title), "whylag %s", WHYLAG_VERSION);

    HWND hwnd = CreateWindowExA(0, wc.lpszClassName,
        title,
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 980, 720,
        NULL, NULL, hInst, NULL);
    SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)wc.hIcon);
    SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)wc.hIconSm);
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return (int)msg.wParam;
}
