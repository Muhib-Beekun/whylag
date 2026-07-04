/*
 * whylag-gui — Win32 GUI for whylag
 * https://github.com/Muhib-Beekun/whylag
 *
 * MIT License — see LICENSE file.
 */

#define _WIN32_WINNT 0x0602
#include "whylag_core.h"
#include <commctrl.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

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
#define IDC_COMPARE2    1015
#define IDT_REFRESH     2001

#define WM_SAMPLE_DONE  (WM_USER + 1)

static HWND g_hwnd;
static HWND g_status, g_counters, g_verdict;
static HWND g_tab, g_list_dpc, g_list_isr, g_list_cpu, g_list_fault;
static HWND g_duration, g_continuous;
static HANDLE g_sample_thread;
static volatile int g_sampling;
static double g_last_elapsed;
static char g_last_csv[MAX_PATH];

static void set_status(const char *text)
{
    SetWindowTextA(g_status, text);
}

static void format_verdict(char *buf, size_t buflen)
{
    WhyLagDriverStats stats[512];
    int count = 0;
    whylag_copy_driver_stats(stats, 512, &count);

    UINT64 worst_dpc = 0, worst_isr = 0;
    const char *dpc_drv = "—", *isr_drv = "—";
    for (int i = 0; i < count; i++) {
        if (stats[i].dpc_max_us > worst_dpc) {
            worst_dpc = stats[i].dpc_max_us; dpc_drv = stats[i].name;
        }
        if (stats[i].isr_max_us > worst_isr) {
            worst_isr = stats[i].isr_max_us; isr_drv = stats[i].name;
        }
    }

    const char *level = "[OK]";
    if (worst_dpc >= 5000 || worst_isr >= 2000)
        level = "[BAD]";
    else if (worst_dpc >= 1000 || worst_isr >= 500)
        level = "[WARN]";

    snprintf(buf, buflen, "%s  Worst DPC: %llu us (%s)  |  Worst ISR: %llu us (%s)",
             level,
             (unsigned long long)worst_dpc, dpc_drv,
             (unsigned long long)worst_isr, isr_drv);
}

static void init_list_columns(HWND list, const char *kind)
{
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    LVCOLUMNA col = {0};
    col.mask = LVCF_TEXT | LVCF_WIDTH;

    if (strcmp(kind, "driver") == 0) {
        col.pszText = "Driver"; col.cx = 180; ListView_InsertColumn(list, 0, &col);
        col.pszText = "Count"; col.cx = 70; ListView_InsertColumn(list, 1, &col);
        col.pszText = "Max (us)"; col.cx = 80; ListView_InsertColumn(list, 2, &col);
        col.pszText = "Avg (us)"; col.cx = 80; ListView_InsertColumn(list, 3, &col);
    } else if (strcmp(kind, "cpu") == 0) {
        col.pszText = "CPU"; col.cx = 60; ListView_InsertColumn(list, 0, &col);
        col.pszText = "DPC #"; col.cx = 70; ListView_InsertColumn(list, 1, &col);
        col.pszText = "DPC max"; col.cx = 80; ListView_InsertColumn(list, 2, &col);
        col.pszText = "ISR #"; col.cx = 70; ListView_InsertColumn(list, 3, &col);
        col.pszText = "ISR max"; col.cx = 80; ListView_InsertColumn(list, 4, &col);
    } else {
        col.pszText = "Process"; col.cx = 180; ListView_InsertColumn(list, 0, &col);
        col.pszText = "PID"; col.cx = 70; ListView_InsertColumn(list, 1, &col);
        col.pszText = "Faults"; col.cx = 80; ListView_InsertColumn(list, 2, &col);
    }
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
    for (int i = 0; i < sc && shown < 20; i++) {
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
    for (int i = 0; i < sc && shown < 20; i++) {
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
        char label[32];
        snprintf(label, sizeof(label), "CPU %d", i);
        char dpcn[32], dpcm[32], isrn[32], isrm[32];
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
    format_verdict(vbuf, sizeof(vbuf));
    SetWindowTextA(g_verdict, vbuf);
}

static void update_counters(void)
{
    UINT64 total, dpc, isr, faults;
    whylag_get_event_counts(&total, &dpc, &isr, &faults);
    char buf[256];
    snprintf(buf, sizeof(buf),
             "Events: %llu  |  DPC: %llu  |  ISR: %llu  |  Page faults: %llu  |  Modules: %d",
             (unsigned long long)total,
             (unsigned long long)dpc,
             (unsigned long long)isr,
             (unsigned long long)faults,
             whylag_get_module_count());
    SetWindowTextA(g_counters, buf);
}

typedef struct {
    WhyLagOptions opts;
} SampleParams;

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
    Sleep(300);

    g_last_elapsed = elapsed;
    PostMessage(g_hwnd, WM_SAMPLE_DONE, 0, 0);
    free(sp);
    return 0;
}

static void start_sampling(void)
{
    if (g_sampling) return;

    char durbuf[16];
    GetWindowTextA(g_duration, durbuf, sizeof(durbuf));
    int duration = atoi(durbuf);
    if (duration < 1) duration = 10;

    SampleParams *sp = (SampleParams *)calloc(1, sizeof(SampleParams));
    sp->opts.duration = duration;
    sp->opts.interval = 5;
    sp->opts.quiet = 1;
    sp->opts.continuous = (SendMessage(g_continuous, BM_GETCHECK, 0, 0) == BST_CHECKED);

    g_running = 1;
    g_sampling = 1;
    EnableWindow(GetDlgItem(g_hwnd, IDC_START), FALSE);
    EnableWindow(GetDlgItem(g_hwnd, IDC_STOP), TRUE);
    EnableWindow(g_duration, FALSE);
    EnableWindow(g_continuous, FALSE);
    set_status(sp->opts.continuous ? "Sampling continuously..." : "Sampling...");

    g_sample_thread = CreateThread(NULL, 0, sample_thread, sp, 0, NULL);
}

static void stop_sampling(void)
{
    if (!g_sampling) return;
    whylag_request_stop();
    set_status("Stopping...");
}

static void on_sample_done(WPARAM failed)
{
    g_sampling = 0;
    EnableWindow(GetDlgItem(g_hwnd, IDC_START), TRUE);
    EnableWindow(GetDlgItem(g_hwnd, IDC_STOP), FALSE);
    EnableWindow(g_duration, TRUE);
    EnableWindow(g_continuous, TRUE);

    if (failed) {
        set_status("Failed to start trace — run as Administrator.");
        return;
    }

    refresh_lists();
    update_counters();
    set_status("Sample complete.");
}

static void export_csv_dialog(void)
{
    char path[MAX_PATH] = "whylag-report.csv";
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
        char msg[512];
        snprintf(msg, sizeof(msg), "Exported CSV to %s", path);
        set_status(msg);
    }
}

typedef struct { char name[64]; UINT64 max_us; } CsvDriverRow;

static int load_csv_drivers(const char *path, CsvDriverRow *dpc, CsvDriverRow *isr, int max_rows, int *dpc_n, int *isr_n)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[512];
    *dpc_n = *isr_n = 0;
    fgets(line, sizeof(line), f); /* header */
    while (fgets(line, sizeof(line), f)) {
        char section[32], name[64];
        double sample;
        unsigned long long count, max_us;
        if (sscanf(line, "%lf,%31[^,],%63[^,]", &sample, section, name) < 3) continue;
        char *p = strchr(line, ',');
        if (!p) continue;
        /* parse max_us from CSV — column 7 */
        unsigned long long pid_u, cpu_u;
        double avg, pct;
        int n = sscanf(line, "%lf,%31[^,],%63[^,],%lu,%lu,%llu,%llu,%lf,%lf",
                       &sample, section, name, &pid_u, &cpu_u, &count, &max_us, &avg, &pct);
        if (n < 7) continue;
        if (strcmp(section, "dpc") == 0 && *dpc_n < max_rows) {
            strncpy(dpc[*dpc_n].name, name, 63);
            dpc[*dpc_n].max_us = max_us;
            (*dpc_n)++;
        } else if (strcmp(section, "isr") == 0 && *isr_n < max_rows) {
            strncpy(isr[*isr_n].name, name, 63);
            isr[*isr_n].max_us = max_us;
            (*isr_n)++;
        }
    }
    fclose(f);
    return 0;
}

static void compare_csv_dialog(void)
{
    char path1[MAX_PATH] = {0}, path2[MAX_PATH] = {0};
    OPENFILENAMEA ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = "CSV files\0*.csv\0All files\0*.*\0";
    ofn.lpstrFile = path1;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST;
    ofn.lpstrTitle = "Select baseline CSV";

    if (!GetOpenFileNameA(&ofn)) return;

    ofn.lpstrFile = path2;
    ofn.lpstrTitle = "Select comparison CSV (bad period)";
    if (!GetOpenFileNameA(&ofn)) return;

    CsvDriverRow base_dpc[256], base_isr[256], bad_dpc[256], bad_isr[256];
    int bd_n = 0, bi_n = 0, bad_d_n = 0, bad_i_n = 0;
    if (load_csv_drivers(path1, base_dpc, base_isr, 256, &bd_n, &bi_n) != 0 ||
        load_csv_drivers(path2, bad_dpc, bad_isr, 256, &bad_d_n, &bad_i_n) != 0) {
        MessageBoxA(g_hwnd, "Could not read one or both CSV files.", "Compare", MB_OK | MB_ICONERROR);
        return;
    }

    char report[4096] = "Drivers with higher max latency in bad period:\n\n";
    int hits = 0;
    for (int i = 0; i < bad_d_n && hits < 12; i++) {
        UINT64 base_max = 0;
        for (int j = 0; j < bd_n; j++)
            if (strcmp(bad_dpc[i].name, base_dpc[j].name) == 0)
                base_max = base_dpc[j].max_us;
        if (bad_dpc[i].max_us > base_max + 100) {
            char line[256];
            snprintf(line, sizeof(line), "DPC  %s: %llu -> %llu us\n",
                     bad_dpc[i].name,
                     (unsigned long long)base_max,
                     (unsigned long long)bad_dpc[i].max_us);
            strncat(report, line, sizeof(report) - strlen(report) - 1);
            hits++;
        }
    }
    for (int i = 0; i < bad_i_n && hits < 20; i++) {
        UINT64 base_max = 0;
        for (int j = 0; j < bi_n; j++)
            if (strcmp(bad_isr[i].name, base_isr[j].name) == 0)
                base_max = base_isr[j].max_us;
        if (bad_isr[i].max_us > base_max + 100) {
            char line[256];
            snprintf(line, sizeof(line), "ISR  %s: %llu -> %llu us\n",
                     bad_isr[i].name,
                     (unsigned long long)base_max,
                     (unsigned long long)bad_isr[i].max_us);
            strncat(report, line, sizeof(report) - strlen(report) - 1);
            hits++;
        }
    }
    if (hits == 0)
        strncat(report, "(No significant regressions vs baseline.)", sizeof(report) - 1);

    MessageBoxA(g_hwnd, report, "Baseline vs bad-period", MB_OK | MB_ICONINFORMATION);
}

static void layout_lists(RECT rc)
{
    RECT tabrc = rc;
    tabrc.top += 110;
    tabrc.bottom -= 40;
    tabrc.left += 10;
    tabrc.right -= 10;
    MoveWindow(g_tab, tabrc.left, tabrc.top, tabrc.right - tabrc.left, tabrc.bottom - tabrc.top, TRUE);

    RECT inner;
    GetClientRect(g_tab, &inner);
    TabCtrl_AdjustRect(g_tab, FALSE, &inner);
    int w = inner.right - inner.left;
    int h = inner.bottom - inner.top;
    MoveWindow(g_list_dpc, inner.left + 4, inner.top + 4, w - 8, h - 8, TRUE);
    MoveWindow(g_list_isr, inner.left + 4, inner.top + 4, w - 8, h - 8, TRUE);
    MoveWindow(g_list_cpu, inner.left + 4, inner.top + 4, w - 8, h - 8, TRUE);
    MoveWindow(g_list_fault, inner.left + 4, inner.top + 4, w - 8, h - 8, TRUE);
}

static void switch_tab(int index)
{
    ShowWindow(g_list_dpc, index == 0 ? SW_SHOW : SW_HIDE);
    ShowWindow(g_list_isr, index == 1 ? SW_SHOW : SW_HIDE);
    ShowWindow(g_list_cpu, index == 2 ? SW_SHOW : SW_HIDE);
    ShowWindow(g_list_fault, index == 3 ? SW_SHOW : SW_HIDE);
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        g_hwnd = hwnd;
        CreateWindowExA(0, "STATIC", "Duration (sec):", WS_CHILD | WS_VISIBLE,
                        10, 12, 100, 20, hwnd, NULL, NULL, NULL);
        g_duration = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "10",
                                     WS_CHILD | WS_VISIBLE | ES_NUMBER,
                                     115, 10, 50, 22, hwnd, (HMENU)IDC_DURATION, NULL, NULL);
        g_continuous = CreateWindowExA(0, "BUTTON", "Continuous",
                                     WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                     180, 10, 100, 22, hwnd, (HMENU)IDC_CONTINUOUS, NULL, NULL);
        CreateWindowExA(0, "BUTTON", "Start", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        300, 8, 70, 26, hwnd, (HMENU)IDC_START, NULL, NULL);
        CreateWindowExA(0, "BUTTON", "Stop", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        380, 8, 70, 26, hwnd, (HMENU)IDC_STOP, NULL, NULL);
        EnableWindow(GetDlgItem(hwnd, IDC_STOP), FALSE);
        CreateWindowExA(0, "BUTTON", "Export CSV", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        470, 8, 90, 26, hwnd, (HMENU)IDC_EXPORT, NULL, NULL);
        CreateWindowExA(0, "BUTTON", "Compare CSVs", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        570, 8, 100, 26, hwnd, (HMENU)IDC_COMPARE, NULL, NULL);

        g_counters = CreateWindowExA(0, "STATIC", "Events: —", WS_CHILD | WS_VISIBLE,
                                     10, 42, 760, 20, hwnd, (HMENU)IDC_COUNTERS, NULL, NULL);
        g_verdict = CreateWindowExA(0, "STATIC", "Verdict: —", WS_CHILD | WS_VISIBLE,
                                    10, 64, 760, 20, hwnd, (HMENU)IDC_VERDICT, NULL, NULL);
        g_status = CreateWindowExA(0, "STATIC", "Ready.", WS_CHILD | WS_VISIBLE,
                                   10, 86, 760, 20, hwnd, (HMENU)IDC_STATUS, NULL, NULL);

        g_tab = CreateWindowExA(0, WC_TABCONTROLA, "",
                                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                                10, 110, 760, 380, hwnd, (HMENU)IDC_TAB, NULL, NULL);
        TCITEMA ti = {0};
        ti.mask = TCIF_TEXT;
        ti.pszText = "DPC drivers"; TabCtrl_InsertItem(g_tab, 0, &ti);
        ti.pszText = "ISR drivers"; TabCtrl_InsertItem(g_tab, 1, &ti);
        ti.pszText = "Per-CPU"; TabCtrl_InsertItem(g_tab, 2, &ti);
        ti.pszText = "Page faults"; TabCtrl_InsertItem(g_tab, 3, &ti);

        DWORD ex = WS_EX_CLIENTEDGE;
        g_list_dpc = CreateWindowExA(ex, WC_LISTVIEWA, "",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
            0, 0, 100, 100, hwnd, (HMENU)IDC_LIST_DPC, NULL, NULL);
        g_list_isr = CreateWindowExA(ex, WC_LISTVIEWA, "",
            WS_CHILD | LVS_REPORT | LVS_SINGLESEL,
            0, 0, 100, 100, hwnd, (HMENU)IDC_LIST_ISR, NULL, NULL);
        g_list_cpu = CreateWindowExA(ex, WC_LISTVIEWA, "",
            WS_CHILD | LVS_REPORT | LVS_SINGLESEL,
            0, 0, 100, 100, hwnd, (HMENU)IDC_LIST_CPU, NULL, NULL);
        g_list_fault = CreateWindowExA(ex, WC_LISTVIEWA, "",
            WS_CHILD | LVS_REPORT | LVS_SINGLESEL,
            0, 0, 100, 100, hwnd, (HMENU)IDC_LIST_FAULT, NULL, NULL);

        init_list_columns(g_list_dpc, "driver");
        init_list_columns(g_list_isr, "driver");
        init_list_columns(g_list_cpu, "cpu");
        init_list_columns(g_list_fault, "fault");
        switch_tab(0);

        if (!whylag_is_admin()) {
            set_status("Not elevated — Start will fail unless you Run as administrator.");
        }

        SetTimer(hwnd, IDT_REFRESH, 500, NULL);
        return 0;
    }
    case WM_SIZE: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        layout_lists(rc);
        return 0;
    }
    case WM_NOTIFY: {
        NMHDR *nm = (NMHDR *)lp;
        if (nm->idFrom == IDC_TAB && nm->code == TCN_SELCHANGE)
            switch_tab(TabCtrl_GetCurSel(g_tab));
        return 0;
    }
    case WM_TIMER:
        if (wp == IDT_REFRESH && g_sampling)
            update_counters();
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
        }
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, IDT_REFRESH);
        whylag_request_stop();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmdLine, int show)
{
    (void)hPrev; (void)cmdLine;
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSEXA wc = {0};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "WhyLagGuiClass";
    RegisterClassExA(&wc);

    HWND hwnd = CreateWindowExA(0, wc.lpszClassName,
        "whylag — system lag diagnostics",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 800, 560,
        NULL, NULL, hInst, NULL);
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return (int)msg.wParam;
}
