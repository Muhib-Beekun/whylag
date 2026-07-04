/*
 * whylag_core.h — shared ETW tracing engine for CLI and GUI
 * MIT License — see LICENSE file.
 */

#ifndef WHYLAG_CORE_H
#define WHYLAG_CORE_H

#define WHYLAG_VERSION "0.2.0"

#define WHYLAG_MAX_CPUS 256

#include <windows.h>

typedef struct {
    int duration;           /* seconds (default 10) */
    int continuous;         /* run until stopped */
    int interval;           /* snapshot interval in continuous mode */
    int quiet;              /* suppress progress output */
    const char *csv_path;   /* optional CSV export path */
} WhyLagOptions;

typedef struct {
    UINT64 dpc_count;
    UINT64 dpc_total_us;
    UINT64 dpc_max_us;
    UINT64 isr_count;
    UINT64 isr_total_us;
    UINT64 isr_max_us;
} WhyLagCpuStats;

typedef struct {
    char     name[64];
    UINT64   dpc_count;
    UINT64   dpc_total_us;
    UINT64   dpc_max_us;
    UINT64   isr_count;
    UINT64   isr_total_us;
    UINT64   isr_max_us;
} WhyLagDriverStats;

typedef struct {
    ULONG  pid;
    char   name[64];
    UINT64 fault_count;
} WhyLagProcFaults;

extern volatile LONG g_running;

int  whylag_is_admin(void);
int  whylag_start_session(int quiet);
void whylag_stop_session(void);
void whylag_request_stop(void);

void whylag_get_event_counts(UINT64 *total, UINT64 *dpc, UINT64 *isr, UINT64 *faults);
int  whylag_get_module_count(void);
int  whylag_get_driver_count(void);
int  whylag_get_cpu_count(void);

void whylag_copy_driver_stats(WhyLagDriverStats *out, int max_count, int *out_count);
void whylag_copy_cpu_stats(WhyLagCpuStats *out, int max_count, int *out_count);
void whylag_copy_fault_stats(WhyLagProcFaults *out, int max_count, int *out_count);

void whylag_print_report(double elapsed_sec, int is_interval);
int  whylag_export_csv(double elapsed_sec, const char *path);

/* Blocking sampling loop; returns 0 on success */
int  whylag_run_loop(const WhyLagOptions *opts, double *elapsed_out);

void whylag_print_usage(void);

#endif /* WHYLAG_CORE_H */
