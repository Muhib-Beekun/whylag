/*
 * whylag_core.c — ETW tracing engine (shared by CLI and GUI)
 * MIT License — see LICENSE file.
 */

#define _WIN32_WINNT 0x0602
#define INITGUID
#include "whylag_core.h"
#include <evntrace.h>
#include <evntcons.h>
#include <tdh.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <shlobj.h>
#include <shellapi.h>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "tdh.lib")
#pragma comment(lib, "shell32.lib")

/* NT internals for kernel module enumeration */

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
typedef NTSTATUS (NTAPI *NtQuerySystemInformation_t)(ULONG, PVOID, ULONG, PULONG);

#define SystemModuleInformation 11
#define STATUS_INFO_LENGTH_MISMATCH ((NTSTATUS)0xC0000004L)

DEFINE_GUID(PerfInfoGuid,
    0xce1dbfb4, 0x137e, 0x4da6, 0x87, 0xb0, 0x3f, 0x59, 0xaa, 0x10, 0x2c, 0xbc);

DEFINE_GUID(PageFaultGuid,
    0x3d6fa8d3, 0xfe05, 0x11d0, 0x9d, 0xda, 0x00, 0xc0, 0x4f, 0xd7, 0xba, 0x7c);

DEFINE_GUID(KernelDiskGuid,
    0x3d6fa8d4, 0xfe05, 0x11d0, 0x9d, 0xda, 0x00, 0xc0, 0x4f, 0xd7, 0xba, 0x7c);

#define OP_LOAD_IMAGE     10
#define OP_UNLOAD_IMAGE   11
#define OP_THREADED_DPC   66
#define OP_ISR            67
#define OP_DPC            68
#define OP_TIMER_DPC      69
#define OP_ISR_LEGACY     50
#define OP_HARDFAULT      32

#ifndef EVENT_TRACE_SYSTEM_LOGGER_MODE
#define EVENT_TRACE_SYSTEM_LOGGER_MODE 0x02000000
#endif

#ifndef EVENT_TRACE_FLAG_IMAGE
#define EVENT_TRACE_FLAG_IMAGE 0x00000004
#endif
#ifndef EVENT_TRACE_FLAG_CSWITCH
#define EVENT_TRACE_FLAG_CSWITCH 0x00000010
#endif
#ifndef EVENT_TRACE_FLAG_DISK_IO
#define EVENT_TRACE_FLAG_DISK_IO 0x00000100
#endif

#define OP_CSWITCH        36

#define SESSION_NAME L"WhyLagTrace"
#define MAX_DRIVERS 512
#define MAX_PROCS   256

typedef struct {
    ULONG_PTR base;
    ULONG     size;
    char      name[64];
} KernelModule;

static KernelModule g_modules[4096];
static int          g_module_count;

static WhyLagDriverStats g_stats[MAX_DRIVERS];
static int               g_stats_count;

static WhyLagProcFaults  g_faults[MAX_PROCS];
static int               g_faults_count;

static WhyLagCpuStats    g_cpu[WHYLAG_MAX_CPUS];
static int               g_cpu_count;

static TRACEHANDLE       g_session_handle;
static TRACEHANDLE       g_consumer_handle = INVALID_PROCESSTRACE_HANDLE;
static HANDLE            g_consumer_thread = NULL;
static const wchar_t    *g_active_session_name = SESSION_NAME;
volatile LONG            g_running = 1;
static LARGE_INTEGER     g_qpc_freq;
static CRITICAL_SECTION  g_stats_lock;
static volatile int      g_sample_elapsed_sec;
static volatile int      g_sample_active;

static volatile UINT64   g_total_events;
static volatile UINT64   g_dpc_events;
static volatile UINT64   g_isr_events;
static volatile UINT64   g_hard_faults;
static volatile UINT64   g_cswitch_events;
static volatile UINT64   g_diskio_events;

int whylag_is_admin(void)
{
    BOOL elevated = FALSE;
    HANDLE token = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION te;
        DWORD size = sizeof(te);
        if (GetTokenInformation(token, TokenElevation, &te, sizeof(te), &size))
            elevated = te.TokenIsElevated;
        CloseHandle(token);
    }
    return elevated ? 1 : 0;
}

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
        ULONG_PTR base = (ULONG_PTR)m->ImageBase;
        ULONG_PTR mapped = (ULONG_PTR)m->MappedBase;
        const char *fname = (const char *)m->FullPathName + m->OffsetToFileName;
        if (base) {
            g_modules[g_module_count].base = base;
            g_modules[g_module_count].size = m->ImageSize;
            strncpy(g_modules[g_module_count].name, fname, 63);
            g_modules[g_module_count].name[63] = 0;
            g_module_count++;
        }
        if (mapped && mapped != base && g_module_count < 4096) {
            g_modules[g_module_count].base = mapped;
            g_modules[g_module_count].size = m->ImageSize;
            strncpy(g_modules[g_module_count].name, fname, 63);
            g_modules[g_module_count].name[63] = 0;
            g_module_count++;
        }
    }
    free(mods);
}

static int cmp_module_base(const void *a, const void *b)
{
    const KernelModule *ma = (const KernelModule *)a;
    const KernelModule *mb = (const KernelModule *)b;
    if (ma->base < mb->base) return -1;
    if (ma->base > mb->base) return 1;
    return 0;
}

static void infer_module_sizes(void)
{
    qsort(g_modules, g_module_count, sizeof(KernelModule), cmp_module_base);
    for (int i = 0; i < g_module_count; i++) {
        if (g_modules[i].size > 0) continue;
        if (i + 1 < g_module_count && g_modules[i + 1].base > g_modules[i].base)
            g_modules[i].size = (ULONG)(g_modules[i + 1].base - g_modules[i].base);
        else
            g_modules[i].size = 0x200000;
    }
}

static void load_modules_psapi(void)
{
    HMODULE psapi = LoadLibraryA("psapi.dll");
    if (!psapi) return;

    typedef BOOL (WINAPI *EnumDeviceDrivers_t)(LPVOID *, DWORD, LPDWORD);
    typedef DWORD (WINAPI *GetDeviceDriverBaseNameA_t)(LPVOID, LPSTR, DWORD);
    EnumDeviceDrivers_t pEnum = (EnumDeviceDrivers_t)GetProcAddress(psapi, "EnumDeviceDrivers");
    GetDeviceDriverBaseNameA_t pName = (GetDeviceDriverBaseNameA_t)GetProcAddress(psapi, "GetDeviceDriverBaseNameA");
    if (!pEnum || !pName) { FreeLibrary(psapi); return; }

    LPVOID bases[2048];
    DWORD needed = 0;
    if (!pEnum(bases, sizeof(bases), &needed)) { FreeLibrary(psapi); return; }
    int count = (int)(needed / sizeof(LPVOID));

    for (int i = 0; i < count && g_module_count < 4096; i++) {
        ULONG_PTR base = (ULONG_PTR)bases[i];
        int found = 0;
        for (int j = 0; j < g_module_count; j++) {
            if (g_modules[j].base == base) { found = 1; break; }
        }
        if (found) continue;
        char name[64] = {0};
        if (pName(bases[i], name, sizeof(name)) == 0) continue;
        g_modules[g_module_count].base = base;
        g_modules[g_module_count].size = 0;
        strncpy(g_modules[g_module_count].name, name, 63);
        g_modules[g_module_count].name[63] = 0;
        g_module_count++;
    }
    FreeLibrary(psapi);
    infer_module_sizes();
}

static void module_add_runtime(ULONG_PTR base, ULONG size, const char *name)
{
    if (base == 0) return;
    for (int i = 0; i < g_module_count; i++) {
        if (g_modules[i].base == base) {
            if (size > g_modules[i].size) g_modules[i].size = size;
            if (name && name[0]) {
                strncpy(g_modules[i].name, name, 63);
                g_modules[i].name[63] = 0;
            }
            return;
        }
    }
    if (g_module_count >= 4096) return;
    g_modules[g_module_count].base = base;
    g_modules[g_module_count].size = size ? size : 0x200000;
    if (name && name[0]) {
        strncpy(g_modules[g_module_count].name, name, 63);
        g_modules[g_module_count].name[63] = 0;
    } else {
        strcpy(g_modules[g_module_count].name, "(module)");
    }
    g_module_count++;
}

static void module_remove_runtime(ULONG_PTR base)
{
    for (int i = 0; i < g_module_count; i++) {
        if (g_modules[i].base == base) {
            g_modules[i] = g_modules[g_module_count - 1];
            g_module_count--;
            return;
        }
    }
}

static int module_index_for(ULONG_PTR addr)
{
    for (int i = 0; i < g_module_count; i++) {
        if (addr >= g_modules[i].base &&
            addr < g_modules[i].base + g_modules[i].size)
            return i;
    }
    return -1;
}

static const char *resolve_address(ULONG_PTR addr)
{
    int idx = module_index_for(addr);
    return idx >= 0 ? g_modules[idx].name : "(unknown)";
}

static ULONG_PTR tdh_get_ulongptr(PEVENT_RECORD event, const wchar_t *name)
{
    PROPERTY_DATA_DESCRIPTOR desc = {0};
    desc.PropertyName = (ULONGLONG)(ULONG_PTR)name;
    desc.ArrayIndex = ULONG_MAX;
    ULONG_PTR val = 0;
    ULONG psize = (ULONG)sizeof(val);
    if (TdhGetProperty(event, 0, NULL, 1, &desc, psize, (PBYTE)&val) == ERROR_SUCCESS)
        return val;
    return 0;
}

static void tdh_get_ascii(PEVENT_RECORD event, const wchar_t *name, char *out, size_t outlen)
{
    PROPERTY_DATA_DESCRIPTOR desc = {0};
    desc.PropertyName = (ULONGLONG)(ULONG_PTR)name;
    desc.ArrayIndex = ULONG_MAX;
    ULONG need = 0;
    if (TdhGetPropertySize(event, 0, NULL, 1, &desc, &need) != ERROR_SUCCESS || need == 0 || need > 512)
        return;
    WCHAR *wbuf = (WCHAR *)malloc(need);
    if (!wbuf) return;
    if (TdhGetProperty(event, 0, NULL, 1, &desc, need, (PBYTE)wbuf) == ERROR_SUCCESS)
        WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, out, (int)outlen, NULL, NULL);
    free(wbuf);
}

static void handle_image_event(PEVENT_RECORD event, int load)
{
    ULONG_PTR base = tdh_get_ulongptr(event, L"ImageBase");
    ULONG size = (ULONG)tdh_get_ulongptr(event, L"ImageSize");
    if (!base && event->UserDataLength >= sizeof(ULONG_PTR))
        base = *(ULONG_PTR *)event->UserData;
    if (!size && event->UserDataLength >= sizeof(ULONG_PTR) + sizeof(ULONG))
        size = *(ULONG *)((BYTE *)event->UserData + sizeof(ULONG_PTR));
    if (base == 0) return;

    char name[64] = {0};
    tdh_get_ascii(event, L"FileName", name, sizeof(name));
    if (!name[0]) tdh_get_ascii(event, L"ImageName", name, sizeof(name));
    if (!name[0] && event->UserDataLength > 16) {
        const WCHAR *w = (const WCHAR *)((BYTE *)event->UserData + 16);
        WideCharToMultiByte(CP_UTF8, 0, w, -1, name, (int)sizeof(name), NULL, NULL);
        const char *slash = strrchr(name, '\\');
        if (slash) memmove(name, slash + 1, strlen(slash));
    }

    EnterCriticalSection(&g_stats_lock);
    if (load)
        module_add_runtime(base, size, name);
    else
        module_remove_runtime(base);
    LeaveCriticalSection(&g_stats_lock);
}

/* MOF layout: InitialTime (8) + Routine pointer (8) at offset 8. */
static ULONG_PTR extract_routine_ptr(PEVENT_RECORD event)
{
    BYTE *data = (BYTE *)event->UserData;
    ULONG len = event->UserDataLength;
    ULONG_PTR candidates[8];
    int nc = 0;

    ULONG_PTR tdh = tdh_get_ulongptr(event, L"Routine");
    if (tdh) candidates[nc++] = tdh;

    if (len >= 16) candidates[nc++] = *(ULONG_PTR *)(data + 8);
    if (len >= 24) candidates[nc++] = *(ULONG_PTR *)(data + 16);

    for (int i = 0; i < nc; i++) {
        if (candidates[i] && module_index_for(candidates[i]) >= 0)
            return candidates[i];
    }

    for (ULONG off = 0; off + sizeof(ULONG_PTR) <= len; off += sizeof(ULONG_PTR)) {
        ULONG_PTR a = *(ULONG_PTR *)(data + off);
        if (a >= 0xFFFF000000000000ULL && module_index_for(a) >= 0)
            return a;
    }

    return nc > 0 ? candidates[0] : 0;
}

static WhyLagDriverStats *get_or_create_stats(const char *name)
{
    EnterCriticalSection(&g_stats_lock);
    for (int i = 0; i < g_stats_count; i++)
        if (strcmp(g_stats[i].name, name) == 0) {
            WhyLagDriverStats *s = &g_stats[i];
            LeaveCriticalSection(&g_stats_lock);
            return s;
        }
    if (g_stats_count >= MAX_DRIVERS) {
        LeaveCriticalSection(&g_stats_lock);
        return &g_stats[0];
    }
    WhyLagDriverStats *s = &g_stats[g_stats_count++];
    memset(s, 0, sizeof(*s));
    strncpy(s->name, name, 63);
    LeaveCriticalSection(&g_stats_lock);
    return s;
}

static WhyLagProcFaults *get_or_create_faults(ULONG pid)
{
    EnterCriticalSection(&g_stats_lock);
    for (int i = 0; i < g_faults_count; i++)
        if (g_faults[i].pid == pid) {
            WhyLagProcFaults *p = &g_faults[i];
            LeaveCriticalSection(&g_stats_lock);
            return p;
        }
    if (g_faults_count >= MAX_PROCS) {
        LeaveCriticalSection(&g_stats_lock);
        return &g_faults[0];
    }
    WhyLagProcFaults *p = &g_faults[g_faults_count++];
    memset(p, 0, sizeof(*p));
    p->pid = pid;
    LeaveCriticalSection(&g_stats_lock);

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

static void update_cpu_dpc(UCHAR cpu, UINT64 delta_us)
{
    EnterCriticalSection(&g_stats_lock);
    if ((int)cpu >= g_cpu_count) g_cpu_count = (int)cpu + 1;
    WhyLagCpuStats *c = &g_cpu[cpu];
    c->dpc_count++;
    c->dpc_total_us += delta_us;
    if (delta_us > c->dpc_max_us) c->dpc_max_us = delta_us;
    LeaveCriticalSection(&g_stats_lock);
}

static void update_cpu_isr(UCHAR cpu, UINT64 delta_us)
{
    EnterCriticalSection(&g_stats_lock);
    if ((int)cpu >= g_cpu_count) g_cpu_count = (int)cpu + 1;
    WhyLagCpuStats *c = &g_cpu[cpu];
    c->isr_count++;
    c->isr_total_us += delta_us;
    if (delta_us > c->isr_max_us) c->isr_max_us = delta_us;
    LeaveCriticalSection(&g_stats_lock);
}

static void WINAPI event_callback(PEVENT_RECORD event)
{
    if (!g_running) return;
    InterlockedIncrement64((volatile LONG64 *)&g_total_events);

    UCHAR cpu = event->BufferContext.ProcessorNumber;
    UCHAR opcode = event->EventHeader.EventDescriptor.Opcode;
    UINT64 event_time = event->EventHeader.TimeStamp.QuadPart;

    if (IsEqualGUID(&event->EventHeader.ProviderId, &PageFaultGuid)) {
        if (opcode == OP_HARDFAULT) {
            ULONG pid = event->EventHeader.ProcessId;
            if (pid != 0 && pid != 4 && pid != 0xFFFFFFFF) {
                WhyLagProcFaults *p = get_or_create_faults(pid);
                EnterCriticalSection(&g_stats_lock);
                p->fault_count++;
                LeaveCriticalSection(&g_stats_lock);
                InterlockedIncrement64((volatile LONG64 *)&g_hard_faults);
            }
        }
        return;
    }

    if (IsEqualGUID(&event->EventHeader.ProviderId, &KernelDiskGuid)) {
        InterlockedIncrement64((volatile LONG64 *)&g_diskio_events);
        return;
    }

    if (!IsEqualGUID(&event->EventHeader.ProviderId, &PerfInfoGuid))
        return;

    if (opcode == OP_CSWITCH) {
        InterlockedIncrement64((volatile LONG64 *)&g_cswitch_events);
        return;
    }

    if (opcode == OP_LOAD_IMAGE) {
        handle_image_event(event, 1);
        return;
    }
    if (opcode == OP_UNLOAD_IMAGE) {
        handle_image_event(event, 0);
        return;
    }

    if (opcode == OP_THREADED_DPC || opcode == OP_DPC || opcode == OP_TIMER_DPC) {
        if (event->UserDataLength >= 16) {
            UINT64 initial = *(UINT64 *)event->UserData;
            ULONG_PTR routine = extract_routine_ptr(event);
            UINT64 delta_us = ((event_time - initial) * 1000000) / g_qpc_freq.QuadPart;

            const char *drv = resolve_address(routine);
            WhyLagDriverStats *s = get_or_create_stats(drv);
            EnterCriticalSection(&g_stats_lock);
            s->dpc_count++;
            s->dpc_total_us += delta_us;
            if (delta_us > s->dpc_max_us) s->dpc_max_us = delta_us;
            LeaveCriticalSection(&g_stats_lock);
            update_cpu_dpc(cpu, delta_us);
            InterlockedIncrement64((volatile LONG64 *)&g_dpc_events);
        }
    }
    else if (opcode == OP_ISR || opcode == OP_ISR_LEGACY) {
        if (event->UserDataLength >= 16) {
            UINT64 initial = *(UINT64 *)event->UserData;
            ULONG_PTR routine = extract_routine_ptr(event);
            UINT64 delta_us = ((event_time - initial) * 1000000) / g_qpc_freq.QuadPart;

            const char *drv = resolve_address(routine);
            WhyLagDriverStats *s = get_or_create_stats(drv);
            EnterCriticalSection(&g_stats_lock);
            s->isr_count++;
            s->isr_total_us += delta_us;
            if (delta_us > s->isr_max_us) s->isr_max_us = delta_us;
            LeaveCriticalSection(&g_stats_lock);
            update_cpu_isr(cpu, delta_us);
            InterlockedIncrement64((volatile LONG64 *)&g_isr_events);
        }
    }
}

static int start_trace_session(void)
{
    ULONG bufsize = sizeof(EVENT_TRACE_PROPERTIES) + 512;
    EVENT_TRACE_PROPERTIES *props = (EVENT_TRACE_PROPERTIES *)calloc(1, bufsize);
    EVENT_TRACE_PROPERTIES *stop_props;
    int use_modern = 1;
    ULONG status;

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

try_start:
    memset(props, 0, bufsize);
    props->Wnode.BufferSize = bufsize;
    props->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    props->Wnode.ClientContext = 1;
    props->EnableFlags = EVENT_TRACE_FLAG_DPC |
                         EVENT_TRACE_FLAG_INTERRUPT |
                         EVENT_TRACE_FLAG_IMAGE |
                         EVENT_TRACE_FLAG_CSWITCH |
                         EVENT_TRACE_FLAG_DISK_IO |
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

    status = StartTraceW(&g_session_handle, name, props);

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
    g_consumer_thread = hThread;
    return 0;
}

static int g_stats_lock_init;

int whylag_start_session(int quiet)
{
    if (!g_stats_lock_init) {
        InitializeCriticalSection(&g_stats_lock);
        g_stats_lock_init = 1;
    }

    QueryPerformanceFrequency(&g_qpc_freq);
    g_running = 1;

    g_stats_count = 0;
    g_faults_count = 0;
    g_cpu_count = 0;
    memset(g_stats, 0, sizeof(g_stats));
    memset(g_faults, 0, sizeof(g_faults));
    memset(g_cpu, 0, sizeof(g_cpu));
    g_total_events = g_dpc_events = g_isr_events = g_hard_faults = 0;
    g_cswitch_events = g_diskio_events = 0;

    load_kernel_modules();
    load_modules_psapi();
    infer_module_sizes();
    if (g_module_count == 0) {
        fprintf(stderr, "warning: could not load kernel modules (addresses won't resolve)\n");
    } else if (!quiet) {
        printf("[+] %d kernel modules loaded\n", g_module_count);
    }

    if (start_trace_session() != 0) return -1;
    if (start_consumer() != 0) { whylag_stop_session(); return -1; }

    if (!quiet) printf("[+] Tracing active\n");
    return 0;
}

void whylag_stop_session(void)
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
    if (g_consumer_thread) {
        WaitForSingleObject(g_consumer_thread, 15000);
        CloseHandle(g_consumer_thread);
        g_consumer_thread = NULL;
    }
}

void whylag_request_stop(void) { g_running = 0; }

void whylag_get_event_counts(UINT64 *total, UINT64 *dpc, UINT64 *isr, UINT64 *faults)
{
    if (total)  *total  = g_total_events;
    if (dpc)    *dpc    = g_dpc_events;
    if (isr)    *isr    = g_isr_events;
    if (faults) *faults = g_hard_faults;
}

void whylag_get_extra_counts(UINT64 *cswitch, UINT64 *disk_io)
{
    if (cswitch) *cswitch = g_cswitch_events;
    if (disk_io) *disk_io = g_diskio_events;
}

int whylag_get_module_count(void) { return g_module_count; }
int whylag_get_driver_count(void) { return g_stats_count; }
int whylag_get_cpu_count(void) { return g_cpu_count; }

void whylag_copy_driver_stats(WhyLagDriverStats *out, int max_count, int *out_count)
{
    EnterCriticalSection(&g_stats_lock);
    int n = g_stats_count < max_count ? g_stats_count : max_count;
    memcpy(out, g_stats, sizeof(WhyLagDriverStats) * n);
    if (out_count) *out_count = n;
    LeaveCriticalSection(&g_stats_lock);
}

void whylag_copy_cpu_stats(WhyLagCpuStats *out, int max_count, int *out_count)
{
    EnterCriticalSection(&g_stats_lock);
    int n = g_cpu_count < max_count ? g_cpu_count : max_count;
    memcpy(out, g_cpu, sizeof(WhyLagCpuStats) * n);
    if (out_count) *out_count = n;
    LeaveCriticalSection(&g_stats_lock);
}

void whylag_copy_fault_stats(WhyLagProcFaults *out, int max_count, int *out_count)
{
    EnterCriticalSection(&g_stats_lock);
    int n = g_faults_count < max_count ? g_faults_count : max_count;
    memcpy(out, g_faults, sizeof(WhyLagProcFaults) * n);
    if (out_count) *out_count = n;
    LeaveCriticalSection(&g_stats_lock);
}

static int cmp_dpc(const void *a, const void *b) {
    UINT64 ma = ((const WhyLagDriverStats *)a)->dpc_max_us;
    UINT64 mb = ((const WhyLagDriverStats *)b)->dpc_max_us;
    return (mb > ma) - (mb < ma);
}

static int cmp_isr(const void *a, const void *b) {
    UINT64 ma = ((const WhyLagDriverStats *)a)->isr_max_us;
    UINT64 mb = ((const WhyLagDriverStats *)b)->isr_max_us;
    return (mb > ma) - (mb < ma);
}

static void print_cpu_table(void)
{
    if (g_cpu_count == 0) return;

    WhyLagCpuStats sorted[WHYLAG_MAX_CPUS];
    int indices[WHYLAG_MAX_CPUS];
    int n = 0;
    for (int i = 0; i < g_cpu_count; i++) {
        if (g_cpu[i].dpc_count == 0 && g_cpu[i].isr_count == 0) continue;
        sorted[n] = g_cpu[i];
        indices[n] = i;
        n++;
    }
    if (n == 0) return;

    /* sort copy by dpc max */
    for (int i = 0; i < n - 1; i++)
        for (int j = i + 1; j < n; j++)
            if (sorted[j].dpc_max_us > sorted[i].dpc_max_us) {
                WhyLagCpuStats ts = sorted[i]; sorted[i] = sorted[j]; sorted[j] = ts;
                int ti = indices[i]; indices[i] = indices[j]; indices[j] = ti;
            }

    printf("  %-28s %8s %9s %9s %9s\n", "CPU", "DPC#", "DPC max", "ISR#", "ISR max");
    printf("  %-28s %8s %9s %9s %9s\n", "---", "----", "-------", "----", "-------");
    int shown = 0;
    for (int i = 0; i < n && shown < 16; i++) {
        char label[32];
        snprintf(label, sizeof(label), "CPU %d", indices[i]);
        printf("  %-28s %8llu %9llu %9llu %9llu\n",
               label,
               (unsigned long long)sorted[i].dpc_count,
               (unsigned long long)sorted[i].dpc_max_us,
               (unsigned long long)sorted[i].isr_count,
               (unsigned long long)sorted[i].isr_max_us);
        shown++;
    }
    printf("\n");
}

void whylag_print_report(double elapsed_sec, int is_interval)
{
    printf("\n");
    if (!is_interval) {
        printf("============================================================\n");
        printf("  WHYLAG REPORT  (%.1f seconds sampled)\n", elapsed_sec);
        printf("============================================================\n");
    } else {
        printf("--- snapshot at %.0fs ", elapsed_sec);
        for (int i = 0; i < 40; i++) putchar('-');
        printf("\n");
    }

    printf("  Events: %llu total | DPC: %llu | ISR: %llu | PageFaults: %llu | Modules: %d\n",
           (unsigned long long)g_total_events,
           (unsigned long long)g_dpc_events,
           (unsigned long long)g_isr_events,
           (unsigned long long)g_hard_faults,
           g_module_count);

    if (!is_interval)
        printf("============================================================\n");
    printf("\n");

    WhyLagDriverStats sorted[MAX_DRIVERS];
    memcpy(sorted, g_stats, sizeof(WhyLagDriverStats) * g_stats_count);
    qsort(sorted, g_stats_count, sizeof(WhyLagDriverStats), cmp_dpc);

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

    memcpy(sorted, g_stats, sizeof(WhyLagDriverStats) * g_stats_count);
    qsort(sorted, g_stats_count, sizeof(WhyLagDriverStats), cmp_isr);

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

    print_cpu_table();

    if (g_hard_faults > 0) {
        for (int i = 0; i < g_faults_count - 1; i++)
            for (int j = i + 1; j < g_faults_count; j++)
                if (g_faults[j].fault_count > g_faults[i].fault_count) {
                    WhyLagProcFaults tmp = g_faults[i];
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

int whylag_export_csv(double elapsed_sec, const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "error: cannot write CSV to %s\n", path);
        return -1;
    }

    fprintf(f, "sample_seconds,section,name,pid,cpu,count,max_us,avg_us,total_pct\n");
    fprintf(f, "%.1f,summary,,,,%llu,,,\n",
            elapsed_sec, (unsigned long long)g_total_events);
    fprintf(f, "%.1f,summary_dpc,,,,%llu,,,\n",
            elapsed_sec, (unsigned long long)g_dpc_events);
    fprintf(f, "%.1f,summary_isr,,,,%llu,,,\n",
            elapsed_sec, (unsigned long long)g_isr_events);
    fprintf(f, "%.1f,summary_fault,,,,%llu,,,\n",
            elapsed_sec, (unsigned long long)g_hard_faults);
    fprintf(f, "%.1f,summary_cswitch,,,,%llu,,,\n",
            elapsed_sec, (unsigned long long)g_cswitch_events);
    fprintf(f, "%.1f,summary_disk,,,,%llu,,,\n",
            elapsed_sec, (unsigned long long)g_diskio_events);

    UINT64 total_dpc_us = 0, total_isr_us = 0;
    for (int i = 0; i < g_stats_count; i++) {
        total_dpc_us += g_stats[i].dpc_total_us;
        total_isr_us += g_stats[i].isr_total_us;
    }

    for (int i = 0; i < g_stats_count; i++) {
        if (g_stats[i].dpc_count == 0) continue;
        UINT64 avg = g_stats[i].dpc_total_us / g_stats[i].dpc_count;
        double pct = total_dpc_us ? (g_stats[i].dpc_total_us * 100.0 / total_dpc_us) : 0;
        fprintf(f, "%.1f,dpc,%s,,%llu,%llu,%llu,%.1f\n",
                elapsed_sec, g_stats[i].name,
                (unsigned long long)g_stats[i].dpc_count,
                (unsigned long long)g_stats[i].dpc_max_us,
                (unsigned long long)avg, pct);
    }

    for (int i = 0; i < g_stats_count; i++) {
        if (g_stats[i].isr_count == 0) continue;
        UINT64 avg = g_stats[i].isr_total_us / g_stats[i].isr_count;
        double pct = total_isr_us ? (g_stats[i].isr_total_us * 100.0 / total_isr_us) : 0;
        fprintf(f, "%.1f,isr,%s,,%llu,%llu,%llu,%.1f\n",
                elapsed_sec, g_stats[i].name,
                (unsigned long long)g_stats[i].isr_count,
                (unsigned long long)g_stats[i].isr_max_us,
                (unsigned long long)avg, pct);
    }

    for (int i = 0; i < g_cpu_count; i++) {
        if (g_cpu[i].dpc_count == 0 && g_cpu[i].isr_count == 0) continue;
        char label[32];
        snprintf(label, sizeof(label), "CPU %d", i);
        UINT64 dpc_avg = g_cpu[i].dpc_count ? g_cpu[i].dpc_total_us / g_cpu[i].dpc_count : 0;
        UINT64 isr_avg = g_cpu[i].isr_count ? g_cpu[i].isr_total_us / g_cpu[i].isr_count : 0;
        fprintf(f, "%.1f,cpu_dpc,%s,,%d,%llu,%llu,,\n",
                elapsed_sec, label, i,
                (unsigned long long)g_cpu[i].dpc_count,
                (unsigned long long)g_cpu[i].dpc_max_us);
        fprintf(f, "%.1f,cpu_isr,%s,,%d,%llu,%llu,,\n",
                elapsed_sec, label, i,
                (unsigned long long)g_cpu[i].isr_count,
                (unsigned long long)g_cpu[i].isr_max_us);
        (void)dpc_avg; (void)isr_avg;
    }

    for (int i = 0; i < g_faults_count; i++) {
        if (g_faults[i].fault_count == 0) continue;
        fprintf(f, "%.1f,fault,%s,%lu,,%llu,,,\n",
                elapsed_sec, g_faults[i].name,
                (unsigned long)g_faults[i].pid,
                (unsigned long long)g_faults[i].fault_count);
    }

    fclose(f);
    return 0;
}

static void sigint_handler(int sig) { (void)sig; g_running = 0; }

void whylag_print_usage(void)
{
    printf("whylag %s — find out why your system is lagging\n\n", WHYLAG_VERSION);
    printf("Usage: whylag [OPTIONS] [DURATION]\n\n");
    printf("Options:\n");
    printf("  -c, --continuous     Run until Ctrl+C (ignore duration)\n");
    printf("  -i, --interval SEC   Report interval in continuous mode (default: 5)\n");
    printf("  -o, --csv FILE       Export report to CSV (baseline vs bad-period diff)\n");
    printf("  -q, --quiet          Suppress progress line, only show reports\n");
    printf("  -h, --help           Show this help\n");
    printf("  -v, --version        Show version\n\n");
    printf("Examples:\n");
    printf("  whylag               Sample for 10 seconds, print report\n");
    printf("  whylag 30            Sample for 30 seconds\n");
    printf("  whylag -o baseline.csv 30\n");
    printf("  whylag -c            Monitor continuously until Ctrl+C\n");
    printf("  whylag -c -i 10      Continuous, snapshot every 10 seconds\n");
    printf("  whylag compare a.csv b.csv   Diff max latency between two exports\n\n");
    printf("Must be run as Administrator (ETW kernel tracing requires it).\n\n");
    printf("Reading results:\n");
    printf("  Max(us) is the key column — worst single DPC/ISR latency in the sample.\n");
    printf("  OK/WARN/BAD thresholds: DPC <1000/<5000 us, ISR <500/<2000 us.\n");
    printf("  Export CSV when fine, capture during stutter, diff max_us per driver.\n");
    printf("  GUI whylag-gui.exe adds Help, Compare CSVs, and live tables.\n");
}

void whylag_get_sample_progress(int *elapsed_sec, int *active)
{
    if (elapsed_sec) *elapsed_sec = g_sample_elapsed_sec;
    if (active) *active = g_sample_active;
}

int whylag_run_loop(const WhyLagOptions *opts, double *elapsed_out)
{
    int duration = opts->duration > 0 ? opts->duration : 10;
    int continuous = opts->continuous;
    int interval = opts->interval > 0 ? opts->interval : 5;
    int quiet = opts->quiet;

    signal(SIGINT, sigint_handler);

    LARGE_INTEGER t_start, t_now;
    QueryPerformanceCounter(&t_start);

    int elapsed_sec = 0;
    int next_report = interval;
    g_sample_active = 1;
    g_sample_elapsed_sec = 0;

    while (g_running) {
        Sleep(1000);
        elapsed_sec++;
        g_sample_elapsed_sec = elapsed_sec;

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

        if (continuous && elapsed_sec >= next_report) {
            whylag_print_report(elapsed, 1);
            next_report += interval;
        }

        if (!continuous && elapsed_sec >= duration) break;
    }

    QueryPerformanceCounter(&t_now);
    double total_elapsed = (double)(t_now.QuadPart - t_start.QuadPart) / g_qpc_freq.QuadPart;
    g_sample_active = 0;
    g_sample_elapsed_sec = elapsed_sec;
    if (elapsed_out) *elapsed_out = total_elapsed;
    return 0;
}

int whylag_lookup_driver(const char *name, WhyLagDriverStats *out)
{
    if (!name || !out) return 0;
    EnterCriticalSection(&g_stats_lock);
    for (int i = 0; i < g_stats_count; i++) {
        if (_stricmp(g_stats[i].name, name) == 0) {
            *out = g_stats[i];
            LeaveCriticalSection(&g_stats_lock);
            return 1;
        }
    }
    LeaveCriticalSection(&g_stats_lock);
    return 0;
}

void whylag_driver_advice(const char *name, char *buf, size_t buflen)
{
    if (!buf || buflen == 0) return;
    buf[0] = 0;
    if (!name) return;

    struct { const char *key; const char *tip; } tips[] = {
        { "nvlddmkm", "Update or roll back the NVIDIA driver. Disable OSD overlays (GeForce Experience, Afterburner)." },
        { "dxgkrnl", "Update GPU drivers. Reduce displays or check multi-GPU configuration." },
        { "dxgmms2", "Part of the graphics memory manager. Update GPU driver with dxgkrnl." },
        { "HDAudBus", "Update audio drivers. Check for IRQ/DPC conflicts with other devices." },
        { "tcpip", "Update NIC driver. Disable NIC power saving in Device Manager." },
        { "ndis", "Update network adapter driver. Check Wi-Fi power management." },
        { "storport", "Check disk health (SMART). Update chipset and storage drivers." },
        { "stornvme", "Update NVMe/firmware. Check thermal throttling on the drive." },
        { "winhvr", "Hyper-V activity. Pause VMs or reduce hypervisor load if not needed." },
        { "vmbusr", "Virtual machine bus. Hyper-V or WSL2 may be active." },
        { "Wdf01000", "Windows Driver Framework. Often a wrapper - look at the parent device driver." },
        { "ntoskrnl", "Kernel core. High counts are normal; focus on unusually high Max (us) values." },
        { "USBPORT", "Disable USB selective suspend. Update chipset/USB drivers." },
        { "CLASSPNP", "Storage class driver. Check disk and HBA drivers underneath." },
        { NULL, "Update or roll back the driver for this device. Compare baseline vs bad-period CSVs." }
    };

    for (int i = 0; tips[i].key; i++) {
        if (_stricmp(name, tips[i].key) == 0 || strstr(name, tips[i].key)) {
            strncpy(buf, tips[i].tip, buflen - 1);
            buf[buflen - 1] = 0;
            return;
        }
    }
    strncpy(buf, tips[sizeof(tips)/sizeof(tips[0]) - 1].tip, buflen - 1);
    buf[buflen - 1] = 0;
}

typedef struct { char name[64]; UINT64 max_us; } CompareRow;

static int load_compare_rows(const char *path, CompareRow *dpc, CompareRow *isr, int max, int *dn, int *in)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[512];
    *dn = *in = 0;
    fgets(line, sizeof(line), f);
    while (fgets(line, sizeof(line), f)) {
        char section[32], name[64];
        double sample, avg, pct;
        unsigned long pid_u;
        unsigned long long cpu_u, count, max_us;
        int n = sscanf(line, "%lf,%31[^,],%63[^,],%lu,%llu,%llu,%llu,%lf,%lf",
                       &sample, section, name, &pid_u, &cpu_u, &count, &max_us, &avg, &pct);
        if (n < 7) continue;
        if (strcmp(section, "dpc") == 0 && *dn < max) {
            strncpy(dpc[*dn].name, name, 63);
            dpc[(*dn)++].max_us = max_us;
        } else if (strcmp(section, "isr") == 0 && *in < max) {
            strncpy(isr[*in].name, name, 63);
            isr[(*in)++].max_us = max_us;
        }
    }
    fclose(f);
    return 0;
}

int whylag_compare_csv(const char *baseline, const char *bad, char *report, size_t report_len)
{
    if (!baseline || !bad || !report || report_len == 0) return -1;
    CompareRow bd[256], bi[256], wd[256], wi[256];
    int bd_n, bi_n, wd_n, wi_n;
    if (load_compare_rows(baseline, bd, bi, 256, &bd_n, &bi_n) != 0 ||
        load_compare_rows(bad, wd, wi, 256, &wd_n, &wi_n) != 0)
        return -1;

    report[0] = 0;
    strncat(report, "Drivers with higher max latency in bad period:\n\n", report_len - 1);
    int hits = 0;
    for (int i = 0; i < wd_n && hits < 12; i++) {
        UINT64 base_max = 0;
        for (int j = 0; j < bd_n; j++)
            if (strcmp(wd[i].name, bd[j].name) == 0) base_max = bd[j].max_us;
        if (wd[i].max_us > base_max + 100) {
            char line[256];
            snprintf(line, sizeof(line), "DPC  %s: %llu -> %llu us\n",
                     wd[i].name, (unsigned long long)base_max, (unsigned long long)wd[i].max_us);
            strncat(report, line, report_len - strlen(report) - 1);
            hits++;
        }
    }
    for (int i = 0; i < wi_n && hits < 20; i++) {
        UINT64 base_max = 0;
        for (int j = 0; j < bi_n; j++)
            if (strcmp(wi[i].name, bi[j].name) == 0) base_max = bi[j].max_us;
        if (wi[i].max_us > base_max + 100) {
            char line[256];
            snprintf(line, sizeof(line), "ISR  %s: %llu -> %llu us\n",
                     wi[i].name, (unsigned long long)base_max, (unsigned long long)wi[i].max_us);
            strncat(report, line, report_len - strlen(report) - 1);
            hits++;
        }
    }
    if (hits == 0)
        strncat(report, "(No significant regressions vs baseline.)\n", report_len - strlen(report) - 1);
    return 0;
}

void whylag_self_check(char *buf, size_t buflen)
{
    if (!buf || buflen == 0) return;
    buf[0] = 0;
    if (!whylag_is_admin())
        strncat(buf, "Not elevated (Run as Admin). ", buflen - 1);
    load_kernel_modules();
    load_modules_psapi();
    infer_module_sizes();
    if (g_module_count == 0)
        strncat(buf, "Could not load kernel module list. ", buflen - 1);
    else {
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "Modules: %d. ", g_module_count);
        strncat(buf, tmp, buflen - 1);
    }
    if (buf[0] == 0)
        strncpy(buf, "Ready to trace.", buflen - 1);
}

static void settings_path(char *out, size_t outlen)
{
    char appdata[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appdata))) {
        snprintf(out, outlen, "%s\\whylag", appdata);
        CreateDirectoryA(out, NULL);
        size_t n = strlen(out);
        snprintf(out + n, outlen - n, "\\settings.ini");
    } else {
        strncpy(out, "whylag-settings.ini", outlen - 1);
    }
}

void whylag_settings_load(WhyLagSettings *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->duration = 10;
    out->live_refresh_ms = 2000;
    out->open_folder_on_export = 1;

    char path[MAX_PATH];
    settings_path(path, sizeof(path));
    out->duration = GetPrivateProfileIntA("ui", "duration", out->duration, path);
    out->live_refresh_ms = GetPrivateProfileIntA("ui", "live_refresh_ms", out->live_refresh_ms, path);
    out->open_folder_on_export = GetPrivateProfileIntA("ui", "open_folder_on_export", 1, path);
    GetPrivateProfileStringA("ui", "last_export_dir", "", out->last_export_dir, MAX_PATH, path);
}

void whylag_settings_save(const WhyLagSettings *in)
{
    if (!in) return;
    char path[MAX_PATH];
    settings_path(path, sizeof(path));
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", in->duration);
    WritePrivateProfileStringA("ui", "duration", buf, path);
    snprintf(buf, sizeof(buf), "%d", in->live_refresh_ms);
    WritePrivateProfileStringA("ui", "live_refresh_ms", buf, path);
    snprintf(buf, sizeof(buf), "%d", in->open_folder_on_export);
    WritePrivateProfileStringA("ui", "open_folder_on_export", buf, path);
    WritePrivateProfileStringA("ui", "last_export_dir", in->last_export_dir, path);
}

void whylag_open_folder_for_file(const char *path)
{
    if (!path || !path[0]) return;
    char dir[MAX_PATH];
    strncpy(dir, path, MAX_PATH - 1);
    char *slash = strrchr(dir, '\\');
    if (slash) *slash = 0;
    if (dir[0])
        ShellExecuteA(NULL, "open", dir, NULL, NULL, SW_SHOW);
}
