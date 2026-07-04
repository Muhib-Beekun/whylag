#include "whylag_help.h"
#include "whylag_gui_theme.h"
#include <dwmapi.h>
#include <stdio.h>
#include <string.h>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

#define IDC_HELP_EDIT  3001
#define IDC_HELP_CLOSE 3002

static const char *WHYLAG_HELP_TEXT =
    "WHYLAG - reading your results\r\n"
    "================================\r\n\r\n"
    "whylag traces kernel Deferred Procedure Calls (DPCs) and Interrupt Service Routines (ISRs).\r\n"
    "These run at elevated priority and block audio, input, and the UI when they run too long.\r\n\r\n"

    "WHAT EACH TAB SHOWS\r\n"
    "-------------------\r\n"
    "DPC drivers   - kernel callbacks scheduled after hardware interrupts. Long DPCs cause\r\n"
    "                mouse stutter, audio dropouts, and UI hitches.\r\n"
    "ISR drivers   - code that runs immediately when hardware fires an interrupt. Should be\r\n"
    "                very short (microseconds). Long ISRs block everything.\r\n"
    "Per-CPU       - which logical CPU saw the worst latency. Useful on multi-core systems\r\n"
    "                when lag feels tied to one core or a NUMA node.\r\n"
    "Page faults   - processes that triggered hard page faults (disk reads mid-operation).\r\n"
    "                These cause multi-millisecond stalls unrelated to drivers.\r\n\r\n"

    "COLUMNS\r\n"
    "-------\r\n"
    "Driver / Process - kernel module (.sys) or process name resolved from the trace.\r\n"
    "Count            - how many events were seen in the sample window.\r\n"
    "Max (us)         - worst single event latency in microseconds. THIS is the key number.\r\n"
    "Avg (us)         - average latency across all events for that driver.\r\n\r\n"

    "VERDICT (OK / WARN / BAD)\r\n"
    "-------------------------\r\n"
    "OK   - DPC max < 1000 us and ISR max < 500 us. Fine for real-time audio.\r\n"
    "WARN - DPC max < 5000 us or ISR max < 2000 us. May glitch at small audio buffers.\r\n"
    "BAD  - Above those limits. Expect audible dropouts or visible stutter.\r\n\r\n"
    "The verdict line shows the worst DPC and worst ISR driver from your sample.\r\n\r\n"

    "WHAT GOOD OUTPUT LOOKS LIKE\r\n"
    "---------------------------\r\n"
    "After a successful sample you should see real driver names (not \"(unknown)\") such as\r\n"
    "ntoskrnl.exe, dxgkrnl.sys, nvlddmkm.sys, HDAudBus.sys, storport.sys, tcpip.sys.\r\n"
    "Many drivers with low Max values is normal. Focus on whichever driver has the highest Max.\r\n\r\n"

    "CSV EXPORT\r\n"
    "----------\r\n"
    "Each CSV contains rows like:\r\n"
    "  section=dpc/isr/cpu_dpc/cpu_isr/fault with name, count, max_us, avg_us\r\n"
    "Use Export CSV for a baseline when the system feels fine, then capture again during\r\n"
    "stutter and use Compare to see which drivers regressed.\r\n\r\n"

    "WORKFLOW - FIND THE CULPRIT\r\n"
    "---------------------------\r\n"
    "1. Baseline  - sample 30 s when everything feels fine; Export CSV.\r\n"
    "2. Capture   - sample during stutter (Continuous mode works well).\r\n"
    "3. Compare   - pick baseline + bad-period CSVs; look for higher max_us.\r\n"
    "4. Fix       - update or roll back the flagged driver (see table below).\r\n\r\n"
    "Double-click any row for driver detail and advice. Opts sets refresh rate and\r\n"
    "export folder behavior. Last sample is saved to AppData and restored on launch.\r\n\r\n"

    "COMMON DRIVERS AND WHAT TO TRY\r\n"
    "------------------------------\r\n"
    "nvlddmkm.sys  - NVIDIA GPU: update/rollback driver; disable overlays (GeForce Experience,\r\n"
    "                MSI Afterburner OSD, etc.).\r\n"
    "dxgkrnl.sys   - Display stack: update GPU driver; reduce displays; check multi-GPU config.\r\n"
    "HDAudBus.sys  - Audio bus: update audio driver; check for IRQ/DPC conflicts.\r\n"
    "tcpip.sys     - Network: update NIC driver; disable NIC power saving.\r\n"
    "storport.sys  - Storage: check disk health; update chipset/storage drivers.\r\n"
    "winhvr.sys    - Hyper-V: pause VMs or reduce hypervisor load when VMs are idle.\r\n"
    "ntoskrnl.exe  - Windows kernel: high counts are normal; worry only if Max is extreme.\r\n\r\n"

    "CLI (whylag.exe)\r\n"
    "----------------\r\n"
    "Same engine as the GUI. Run elevated:\r\n"
    "  whylag 30                  - 30-second sample, print report\r\n"
    "  whylag -o baseline.csv 30  - export CSV\r\n"
    "  whylag -c -i 10            - continuous snapshots every 10 s\r\n"
    "  whylag -h                  - full CLI options\r\n\r\n"

    "NOTES\r\n"
    "-----\r\n"
    "Administrator privileges are required for kernel ETW tracing.\r\n"
    "The GUI prompts for UAC elevation when you launch it. Run whylag.exe elevated for CLI sampling.\r\n"
    "whylag is diagnostic only. It leaves drivers and system settings unchanged.\r\n";

static HWND g_help_hwnd;

static LRESULT CALLBACK help_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

        HWND edit = CreateWindowExA(0, "EDIT", WHYLAG_HELP_TEXT,
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL |
            ES_AUTOVSCROLL | ES_WANTRETURN,
            12, 12, 576, 400, hwnd, (HMENU)IDC_HELP_EDIT, NULL, NULL);
        SendMessage(edit, EM_SETSEL, 0, 0);
        SendMessage(edit, WM_SETFONT, (WPARAM)wl_font_ui(), FALSE);

        CreateWindowExA(0, "BUTTON", "Close",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            498, 422, 90, 28, hwnd, (HMENU)IDC_HELP_CLOSE, NULL, NULL);
        return 0;
    }
    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wp;
        SetTextColor(hdc, WL_CLR_TEXT);
        SetBkColor(hdc, WL_CLR_EDIT);
        return (LRESULT)wl_brush(3);
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_HELP_CLOSE) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        g_help_hwnd = NULL;
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

void whylag_show_help(HWND parent)
{
    if (g_help_hwnd) {
        SetForegroundWindow(g_help_hwnd);
        return;
    }

    static int registered;
    if (!registered) {
        WNDCLASSEXA wc = {0};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = help_wndproc;
        wc.hInstance = GetModuleHandleA(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = "WhyLagHelpClass";
        RegisterClassExA(&wc);
        registered = 1;
    }

    g_help_hwnd = CreateWindowExA(0, "WhyLagHelpClass", "whylag - help",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_SIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 620, 520,
        parent, NULL, GetModuleHandleA(NULL), NULL);
    ShowWindow(g_help_hwnd, SW_SHOW);
}
