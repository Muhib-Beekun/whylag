/*
 * lagmon - Lightweight DPC/ISR latency monitor for Windows
 * https://github.com/nicobailon/lagmon
 *
 * Single-binary, no-install, no-driver latency diagnostics.
 * Uses ETW to capture kernel DPC/ISR events in real-time and attributes
 * them to driver modules via NtQuerySystemInformation.
 *
 * Requires: Windows 8+ and Administrator privileges.
 * Build:    gcc -O2 -o lagmon.exe lagmon.c -ltdh -ladvapi32
 *
 * MIT License — see LICENSE file.
 */

#define LAGMON_VERSION "0.1.0"

#define _WIN32_WINNT 0x0602
#define INITGUID
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <tdh.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "tdh.lib")

/* ================================================================
 * NT internals for kernel module enumeration
 * ================================================================ */

typedef struct _RTL_PROCESS_MODULE_INFORMATION {
    HANDLE Section;
    PVOID  MappedBase;
    PVOID  ImageBase;
    ULONG  ImageSize;
    ULONG  Flags;
    USHORT LoadOrderIndex;
    USHORT InitOrderIndex;
    USHORT LoadCount;
    USHORT OffsetToFileName;
    UCHAR  FullPathName[256];
} RTL_PROCESS_MODULE_INFORMATION;

typedef struct _RTL_PROCESS_MODULES {
    ULONG NumberOfModules;
    RTL_PROCESS_MODULE_INFORMATION Modules[1];
} RTL_PROCESS_MODULES;

typedef LONG NTSTATUS;
typedef NTSTATUS (NTAPI *NtQuerySystemInformation_t)(
    ULONG, PVOID, ULONG, PULONG);

#define SystemModuleInformation 11
#define STATUS_INFO_LENGTH_MISMATCH ((NTSTATUS)0xC0000004L)

/* ================================================================
 * ETW GUIDs and opcodes
 * ================================================================ */

DEFINE_GUID(PerfInfoGuid,
    0xce1dbfb4, 0x137e, 0x4da6, 0x87, 0xb0, 0x3f, 0x59, 0xaa, 0x10, 0x2c, 0xbc);

DEFINE_GUID(PageFaultGuid,
    0x3d6fa8d3, 0xfe05, 0x11d0, 0x9d, 0xda, 0x00, 0xc0, 0x4f, 0xd7, 0xba, 0x7c);

/* PerfInfo opcodes on Windows 8+ (64-bit) */
#define OP_DPC          66  /* DPC complete (ISR-triggered) */
#define OP_TIMER_DPC    67  /* Timer DPC complete */
#define OP_THREADED_DPC 68  /* Threaded DPC (opcode varies) */
#define OP_TDPC_END     69  /* Threaded DPC complete */
#define OP_ISR          50  /* ISR complete */
#define OP_HARDFAULT    32  /* Hard page fault */

#ifndef EVENT_TRACE_SYSTEM_LOGGER_MODE
#define EVENT_TRACE_SYSTEM_LOGGER_MODE 0x02000000
#endif

#define SESSION_NAME L"LagmonTrace"

/* ================================================================
 * Data structures
 * ================================================================ */

#define MAX_DRIVERS 512
#define MAX_PROCS   256

typedef struct {
    ULONG_PTR base;
    ULONG     size;
    char      name[64];
} KernelModule;

typedef struct {
    char     name[64];
    UINT64   dpc_count;
    UINT64   dpc_total_us;
    UINT64   dpc_max_us;
    UINT64   isr_count;
    UINT64   isr_total_us;
    UINT64   isr_max_us;
} DriverStats;

typedef struct {
    ULONG  pid;
    char   name[64];
    UINT64 fault_count;
} ProcFaults;

/* ================================================================
 * Global state
 * ================================================================ */

static KernelModule g_modules[4096];
static int          g_module_count;

static DriverStats  g_stats[MAX_DRIVERS];
static int          g_stats_count;

static ProcFaults   g_faults[MAX_PROCS];
static int          g_faults_count;

static TRACEHANDLE  g_session_handle;
static TRACEHANDLE  g_consumer_handle = INVALID_PROCESSTRACE_HANDLE;
static const wchar_t *g_active_session_name = SESSION_NAME;
static volatile LONG g_running = 1;
static LARGE_INTEGER g_qpc_freq;

static volatile UINT64 g_total_events;
static volatile UINT64 g_dpc_events;
static volatile UINT64 g_isr_events;
static volatile UINT64 g_hard_faults;

/* ================================================================
 * Kernel module resolution
 * ================================================================ */

static void load_kernel_modules(void)
{
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (!ntdll) return;

    NtQuerySystemInformation_t NtQSI =
        (NtQuerySystemInformation_t)GetProcAddress(ntdll, "NtQuerySystemInformation");
    if (!NtQSI) return;

    ULONG bufsize = 256 * 1024;
    RTL_PROCESS_MODULES *mods = NULL;
    NTSTATUS status;

    for (int attempt = 0; attempt < 5; attempt++) {
        mods = (RTL_PROCESS_MODULES *)malloc(bufsize);
        if (!mods) return;
        status = NtQSI(SystemModuleInformation, mods, bufsize, &bufsize);
        if (status == STATUS_INFO_LENGTH_MISMATCH) {
            free(mods); mods = NULL;
            bufsize *= 2;
            continue;
        }
        break;
    }

    if (!mods || status < 0) { free(mods); return; }

    g_module_count = 0;
    for (ULONG i = 0; i < mods->NumberOfModules && g_module_count < 4096; i++) {
        RTL_PROCESS_MODULE_INFORMATION *m = &mods->Modules[i];
        g_modules[g_module_count].base = (ULONG_PTR)m->ImageBase;
        g_modules[g_module_count].size = m->ImageSize;
        const char *fname = (const char *)m->FullPathName + m->OffsetToFileName;
        strncpy(g_modules[g_module_count].name, fname, 63);
        g_modules[g_module_count].name[63] = 0;
        g_module_count++;
    }
    free(mods);
}

static const char *resolve_address(ULONG_PTR addr)
{
    for (int i = 0; i < g_module_count; i++) {
        if (addr >= g_modules[i].base &&
            addr < g_modules[i].base + g_modules[i].size)
            return g_modules[i].name;
    }
    return "(unknown)";
}

/* ================================================================
 * Stats helpers
 * ================================================================ */

static DriverStats *get_or_create_stats(const char *name)
{
    for (int i = 0; i < g_stats_count; i++)
        if (strcmp(g_stats[i].name, name) == 0) return &g_stats[i];
    if (g_stats_count >= MAX_DRIVERS) return &g_stats[0];
    DriverStats *s = &g_stats[g_stats_count++];
    memset(s, 0, sizeof(*s));
    strncpy(s->name, name, 63);
    return s;
}

static ProcFaults *get_or_create_faults(ULONG pid)
{
    for (int i = 0; i < g_faults_count; i++)
        if (g_faults[i].pid == pid) return &g_faults[i];
    if (g_faults_count >= MAX_PROCS) return &g_faults[0];
    ProcFaults *p = &g_faults[g_faults_count++];
    memset(p, 0, sizeof(*p));
    p->pid = pid;
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hProc) {
        char path[MAX_PATH] = {0};
        DWORD pathlen = MAX_PATH;
        if (QueryFullProcessImageNameA(hProc, 0, path, &pathlen)) {
            const char *slash = strrchr(path, '\\');
            strncpy(p->name, slash ? slash + 1 : path, 63);
        } else {
            snprintf(p->name, 63, "PID %lu", (unsigned long)pid);
        }
        CloseHandle(hProc);
    } else {
        snprintf(p->name, 63, "PID %lu", (unsigned long)pid);
    }
    return p;
}

/* ================================================================
 * ETW event callback (hot path)
 * ================================================================ */

static void WINAPI event_callback(PEVENT_RECORD event)
{
    if (!g_running) return;
    InterlockedIncrement64((volatile LONG64 *)&g_total_events);

    UCHAR opcode = event->EventHeader.EventDescriptor.Opcode;
    UINT64 event_time = event->EventHeader.TimeStamp.QuadPart;

    /* Hard page faults */
    if (IsEqualGUID(&event->EventHeader.ProviderId, &PageFaultGuid)) {
        if (opcode == OP_HARDFAULT) {
            ULONG pid = event->EventHeader.ProcessId;
            if (pid != 0 && pid != 4 && pid != 0xFFFFFFFF) {
                ProcFaults *p = get_or_create_faults(pid);
                p->fault_count++;
                InterlockedIncrement64((volatile LONG64 *)&g_hard_faults);
            }
        }
        return;
    }

    /* DPC/ISR events from PerfInfo */
    if (!IsEqualGUID(&event->EventHeader.ProviderId, &PerfInfoGuid))
        return;

    /*
     * Self-contained event layout (64-bit Windows 8+):
     *   [0..7]  UINT64    InitialTime (QPC at routine entry)
     *   [8..15] ULONG_PTR Routine address
     */

    if (opcode == OP_DPC || opcode == OP_TIMER_DPC || opcode == OP_TDPC_END) {
        if (event->UserDataLength >= 16) {
            UINT64 initial = *(UINT64 *)event->UserData;
            ULONG_PTR routine = *(ULONG_PTR *)((BYTE *)event->UserData + 8);
            UINT64 delta_us = ((event_time - initial) * 1000000) / g_qpc_freq.QuadPart;

            const char *drv = resolve_address(routine);
            DriverStats *s = get_or_create_stats(drv);
            s->dpc_count++;
            s->dpc_total_us += delta_us;
            if (delta_us > s->dpc_max_us) s->dpc_max_us = delta_us;
            InterlockedIncrement64((volatile LONG64 *)&g_dpc_events);
        }
    }
    else if (opcode == OP_ISR) {
        if (event->UserDataLength >= 16) {
            UINT64 initial = *(UINT64 *)event->UserData;
            ULONG_PTR routine = *(ULONG_PTR *)((BYTE *)event->UserData + 8);
            UINT64 delta_us = ((event_time - initial) * 1000000) / g_qpc_freq.QuadPart;

            const char *drv = resolve_address(routine);
            DriverStats *s = get_or_create_stats(drv);
            s->isr_count++;
            s->isr_total_us += delta_us;
            if (delta_us > s->isr_max_us) s->isr_max_us = delta_us;
            InterlockedIncrement64((volatile LONG64 *)&g_isr_events);
        }
    }
}

/* ================================================================
 * ETW session management
 * ================================================================ */

static int start_trace_session(void)
{
    ULONG bufsize = sizeof(EVENT_TRACE_PROPERTIES) + 512;
    EVENT_TRACE_PROPERTIES *props = (EVENT_TRACE_PROPERTIES *)calloc(1, bufsize);
    EVENT_TRACE_PROPERTIES *stop_props;

    /* Clean up stale sessions */
    stop_props = (EVENT_TRACE_PROPERTIES *)calloc(1, bufsize);
    stop_props->Wnode.BufferSize = bufsize;
    stop_props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    ControlTraceW(0, SESSION_NAME, stop_props, EVENT_TRACE_CONTROL_STOP);
    free(stop_props);

    stop_props = (EVENT_TRACE_PROPERTIES *)calloc(1, bufsize);
    stop_props->Wnode.BufferSize = bufsize;
    stop_props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    ControlTraceW(0, L"NT Kernel Logger", stop_props, EVENT_TRACE_CONTROL_STOP);
    free(stop_props);
    Sleep(300);

    /* Try Win8+ system logger mode first, fall back to NT Kernel Logger */
    int use_modern = 1;

try_start:
    memset(props, 0, bufsize);
    props->Wnode.BufferSize = bufsize;
    props->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    props->Wnode.ClientContext = 1; /* QPC */
    props->EnableFlags = EVENT_TRACE_FLAG_DPC |
                         EVENT_TRACE_FLAG_INTERRUPT |
                         EVENT_TRACE_FLAG_MEMORY_HARD_FAULTS;
    props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    props->BufferSize = 64;
    props->MinimumBuffers = 32;
    props->MaximumBuffers = 128;
    props->FlushTimer = 1;

    const wchar_t *name;
    if (use_modern) {
        props->LogFileMode = EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_SYSTEM_LOGGER_MODE;
        name = SESSION_NAME;
    } else {
        props->Wnode.Guid = SystemTraceControlGuid;
        props->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
        name = L"NT Kernel Logger";
    }

    ULONG status = StartTraceW(&g_session_handle, name, props);

    if (status == ERROR_INVALID_PARAMETER && use_modern) {
        use_modern = 0;
        goto try_start;
    }
    if (status == ERROR_ALREADY_EXISTS) {
        stop_props = (EVENT_TRACE_PROPERTIES *)calloc(1, bufsize);
        stop_props->Wnode.BufferSize = bufsize;
        stop_props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        ControlTraceW(0, name, stop_props, EVENT_TRACE_CONTROL_STOP);
        free(stop_props);
        Sleep(500);
        status = StartTraceW(&g_session_handle, name, props);
    }

    free(props);

    if (status != ERROR_SUCCESS) {
        if (status == 5)
            fprintf(stderr, "error: access denied — run as Administrator\n");
        else
            fprintf(stderr, "error: StartTrace failed (code %lu)\n", status);
        return -1;
    }

    g_active_session_name = use_modern ? SESSION_NAME : L"NT Kernel Logger";
    return 0;
}

static DWORD WINAPI consumer_thread(LPVOID param)
{
    (void)param;
    ProcessTrace(&g_consumer_handle, 1, NULL, NULL);
    return 0;
}

static int start_consumer(void)
{
    EVENT_TRACE_LOGFILEW logfile = {0};
    logfile.LoggerName = (LPWSTR)g_active_session_name;
    logfile.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME |
                               PROCESS_TRACE_MODE_EVENT_RECORD |
                               PROCESS_TRACE_MODE_RAW_TIMESTAMP;
    logfile.EventRecordCallback = event_callback;

    g_consumer_handle = OpenTraceW(&logfile);
    if (g_consumer_handle == INVALID_PROCESSTRACE_HANDLE) {
        fprintf(stderr, "error: OpenTrace failed (%lu)\n", GetLastError());
        return -1;
    }

    HANDLE hThread = CreateThread(NULL, 0, consumer_thread, NULL, 0, NULL);
    if (!hThread) { fprintf(stderr, "error: CreateThread failed\n"); return -1; }
    CloseHandle(hThread);
    return 0;
}

static void stop_session(void)
{
    ULONG bufsize = sizeof(EVENT_TRACE_PROPERTIES) + 512;
    EVENT_TRACE_PROPERTIES *props = (EVENT_TRACE_PROPERTIES *)calloc(1, bufsize);
    props->Wnode.BufferSize = bufsize;
    props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    ControlTraceW(0, g_active_session_name, props, EVENT_TRACE_CONTROL_STOP);
    free(props);
    if (g_consumer_handle != INVALID_PROCESSTRACE_HANDLE) {
        CloseTrace(g_consumer_handle);
        g_consumer_handle = INVALID_PROCESSTRACE_HANDLE;
    }
}

/* ================================================================
 * Reporting
 * ================================================================ */

static int cmp_dpc(const void *a, const void *b) {
    UINT64 ma = ((const DriverStats *)a)->dpc_max_us;
    UINT64 mb = ((const DriverStats *)b)->dpc_max_us;
    return (mb > ma) - (mb < ma);
}

static int cmp_isr(const void *a, const void *b) {
    UINT64 ma = ((const DriverStats *)a)->isr_max_us;
    UINT64 mb = ((const DriverStats *)b)->isr_max_us;
    return (mb > ma) - (mb < ma);
}

static void print_report(double elapsed_sec, int is_interval)
{
    printf("\n");
    if (!is_interval) {
        printf("============================================================\n");
        printf("  LAGMON REPORT  (%.1f seconds sampled)\n", elapsed_sec);
        printf("============================================================\n");
    } else {
        printf("--- snapshot at %.0fs ", elapsed_sec);
        for (int i = 0; i < 40; i++) putchar('-');
        printf("\n");
    }

    printf("  Events: %llu total | DPC: %llu | ISR: %llu | PageFaults: %llu\n",
           (unsigned long long)g_total_events,
           (unsigned long long)g_dpc_events,
           (unsigned long long)g_isr_events,
           (unsigned long long)g_hard_faults);

    if (!is_interval)
        printf("============================================================\n");
    printf("\n");

    /* DPC table */
    DriverStats sorted[MAX_DRIVERS];
    memcpy(sorted, g_stats, sizeof(DriverStats) * g_stats_count);
    qsort(sorted, g_stats_count, sizeof(DriverStats), cmp_dpc);

    printf("  %-28s %8s %9s %9s %8s\n", "DRIVER (DPC)", "Count", "Max(us)", "Avg(us)", "Total%");
    printf("  %-28s %8s %9s %9s %8s\n", "------------", "-----", "-------", "-------", "------");
    int shown = 0;
    UINT64 total_dpc_us = 0;
    for (int i = 0; i < g_stats_count; i++) total_dpc_us += g_stats[i].dpc_total_us;

    for (int i = 0; i < g_stats_count && shown < 10; i++) {
        if (sorted[i].dpc_count == 0) continue;
        UINT64 avg = sorted[i].dpc_total_us / sorted[i].dpc_count;
        double pct = total_dpc_us ? (sorted[i].dpc_total_us * 100.0 / total_dpc_us) : 0;
        printf("  %-28s %8llu %9llu %9llu %7.1f%%\n",
               sorted[i].name,
               (unsigned long long)sorted[i].dpc_count,
               (unsigned long long)sorted[i].dpc_max_us,
               (unsigned long long)avg, pct);
        shown++;
    }
    if (shown == 0) printf("  (no DPC events captured)\n");
    printf("\n");

    /* ISR table */
    memcpy(sorted, g_stats, sizeof(DriverStats) * g_stats_count);
    qsort(sorted, g_stats_count, sizeof(DriverStats), cmp_isr);

    printf("  %-28s %8s %9s %9s %8s\n", "DRIVER (ISR)", "Count", "Max(us)", "Avg(us)", "Total%");
    printf("  %-28s %8s %9s %9s %8s\n", "------------", "-----", "-------", "-------", "------");
    shown = 0;
    UINT64 total_isr_us = 0;
    for (int i = 0; i < g_stats_count; i++) total_isr_us += g_stats[i].isr_total_us;

    for (int i = 0; i < g_stats_count && shown < 10; i++) {
        if (sorted[i].isr_count == 0) continue;
        UINT64 avg = sorted[i].isr_total_us / sorted[i].isr_count;
        double pct = total_isr_us ? (sorted[i].isr_total_us * 100.0 / total_isr_us) : 0;
        printf("  %-28s %8llu %9llu %9llu %7.1f%%\n",
               sorted[i].name,
               (unsigned long long)sorted[i].isr_count,
               (unsigned long long)sorted[i].isr_max_us,
               (unsigned long long)avg, pct);
        shown++;
    }
    if (shown == 0) printf("  (no ISR events captured)\n");
    printf("\n");

    /* Page faults */
    if (g_hard_faults > 0) {
        for (int i = 0; i < g_faults_count - 1; i++)
            for (int j = i + 1; j < g_faults_count; j++)
                if (g_faults[j].fault_count > g_faults[i].fault_count) {
                    ProcFaults tmp = g_faults[i];
                    g_faults[i] = g_faults[j];
                    g_faults[j] = tmp;
                }

        printf("  %-28s %8s %9s\n", "PROCESS (Hard Faults)", "PID", "Faults");
        printf("  %-28s %8s %9s\n", "---------------------", "---", "------");
        for (int i = 0; i < g_faults_count && i < 8; i++) {
            if (g_faults[i].fault_count == 0) break;
            printf("  %-28s %8lu %9llu\n",
                   g_faults[i].name,
                   (unsigned long)g_faults[i].pid,
                   (unsigned long long)g_faults[i].fault_count);
        }
        printf("\n");
    }

    /* Verdict (only on final report) */
    if (!is_interval) {
        UINT64 worst_dpc = 0, worst_isr = 0;
        const char *dpc_drv = "—", *isr_drv = "—";
        for (int i = 0; i < g_stats_count; i++) {
            if (g_stats[i].dpc_max_us > worst_dpc) {
                worst_dpc = g_stats[i].dpc_max_us; dpc_drv = g_stats[i].name;
            }
            if (g_stats[i].isr_max_us > worst_isr) {
                worst_isr = g_stats[i].isr_max_us; isr_drv = g_stats[i].name;
            }
        }

        printf("  VERDICT: ");
        if (worst_dpc < 1000 && worst_isr < 500)
            printf("[OK] Suitable for real-time audio.\n");
        else if (worst_dpc < 5000 && worst_isr < 2000)
            printf("[WARN] Moderate latency — may cause occasional glitches.\n");
        else
            printf("[BAD] High latency — will cause audio dropouts/stuttering.\n");

        printf("    Worst DPC: %llu us (%s)\n", (unsigned long long)worst_dpc, dpc_drv);
        printf("    Worst ISR: %llu us (%s)\n", (unsigned long long)worst_isr, isr_drv);
        printf("    Thresholds: DPC <1000/<5000/>5000 us | ISR <500/<2000/>2000 us\n");
        printf("============================================================\n");
    }
}

/* ================================================================
 * Main
 * ================================================================ */

static void sigint_handler(int sig) { (void)sig; g_running = 0; }

static void print_usage(void)
{
    printf("lagmon %s — DPC/ISR latency monitor (no driver required)\n\n", LAGMON_VERSION);
    printf("Usage: lagmon [OPTIONS] [DURATION]\n\n");
    printf("Options:\n");
    printf("  -c, --continuous     Run until Ctrl+C (ignore duration)\n");
    printf("  -i, --interval SEC   Report interval in continuous mode (default: 5)\n");
    printf("  -q, --quiet          Suppress progress line, only show reports\n");
    printf("  -h, --help           Show this help\n");
    printf("  -v, --version        Show version\n\n");
    printf("Examples:\n");
    printf("  lagmon               Sample for 10 seconds, print report\n");
    printf("  lagmon 30            Sample for 30 seconds\n");
    printf("  lagmon -c            Monitor continuously until Ctrl+C\n");
    printf("  lagmon -c -i 10      Continuous, snapshot every 10 seconds\n\n");
    printf("Must be run as Administrator (ETW kernel tracing requires it).\n");
}

int main(int argc, char **argv)
{
    int duration = 10;
    int continuous = 0;
    int interval = 5;
    int quiet = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(); return 0;
        }
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("lagmon %s\n", LAGMON_VERSION); return 0;
        }
        if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--continuous") == 0) {
            continuous = 1; continue;
        }
        if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
            quiet = 1; continue;
        }
        if ((strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--interval") == 0) && i + 1 < argc) {
            interval = atoi(argv[++i]);
            if (interval < 1) interval = 5;
            continue;
        }
        int d = atoi(argv[i]);
        if (d > 0) duration = d;
    }

    printf("lagmon %s — DPC/ISR latency monitor\n", LAGMON_VERSION);
    if (continuous)
        printf("Continuous mode (Ctrl+C to stop, reporting every %ds)\n\n", interval);
    else
        printf("Sampling for %d seconds (Ctrl+C to stop early)...\n\n", duration);

    QueryPerformanceFrequency(&g_qpc_freq);
    signal(SIGINT, sigint_handler);

    load_kernel_modules();
    if (g_module_count == 0) {
        fprintf(stderr, "warning: could not load kernel modules (addresses won't resolve)\n");
    } else if (!quiet) {
        printf("[+] %d kernel modules loaded\n", g_module_count);
    }

    if (start_trace_session() != 0) return 1;
    if (start_consumer() != 0) { stop_session(); return 1; }

    if (!quiet) printf("[+] Tracing active\n");

    LARGE_INTEGER t_start, t_now;
    QueryPerformanceCounter(&t_start);

    int elapsed_sec = 0;
    int next_report = interval;

    while (g_running) {
        Sleep(1000);
        elapsed_sec++;

        QueryPerformanceCounter(&t_now);
        double elapsed = (double)(t_now.QuadPart - t_start.QuadPart) / g_qpc_freq.QuadPart;

        if (!quiet && !continuous) {
            printf("\r  [%d/%d] DPC:%llu ISR:%llu Faults:%llu",
                   elapsed_sec, duration,
                   (unsigned long long)g_dpc_events,
                   (unsigned long long)g_isr_events,
                   (unsigned long long)g_hard_faults);
            fflush(stdout);
        } else if (!quiet && continuous) {
            printf("\r  [%ds] DPC:%llu ISR:%llu Faults:%llu       ",
                   elapsed_sec,
                   (unsigned long long)g_dpc_events,
                   (unsigned long long)g_isr_events,
                   (unsigned long long)g_hard_faults);
            fflush(stdout);
        }

        /* Periodic snapshot in continuous mode */
        if (continuous && elapsed_sec >= next_report) {
            print_report(elapsed, 1);
            next_report += interval;
        }

        /* Stop after duration in non-continuous mode */
        if (!continuous && elapsed_sec >= duration) break;
    }

    QueryPerformanceCounter(&t_now);
    double total_elapsed = (double)(t_now.QuadPart - t_start.QuadPart) / g_qpc_freq.QuadPart;

    g_running = 0;
    stop_session();
    Sleep(300);

    print_report(total_elapsed, 0);
    return 0;
}
