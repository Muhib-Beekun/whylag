/*
 * whylag_core.h — shared ETW tracing engine for CLI and GUI
 * MIT License — see LICENSE file.
 */

#ifndef WHYLAG_CORE_H
#define WHYLAG_CORE_H

#define WHYLAG_VERSION "0.3.0"

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

typedef struct {
    int duration;
    int live_refresh_ms;
    int open_folder_on_export;
    int last_sample_elapsed_sec;
    char last_export_dir[MAX_PATH];
} WhyLagSettings;

extern volatile LONG g_running;

int  whylag_is_admin(void);
int  whylag_start_session(int quiet);
void whylag_stop_session(void);
void whylag_request_stop(void);

void whylag_get_event_counts(UINT64 *total, UINT64 *dpc, UINT64 *isr, UINT64 *faults);
void whylag_get_extra_counts(UINT64 *cswitch, UINT64 *disk_io);
void whylag_get_sample_progress(int *elapsed_sec, int *active);
int  whylag_get_module_count(void);
int  whylag_get_driver_count(void);
int  whylag_get_cpu_count(void);

void whylag_copy_driver_stats(WhyLagDriverStats *out, int max_count, int *out_count);
void whylag_copy_cpu_stats(WhyLagCpuStats *out, int max_count, int *out_count);
void whylag_copy_fault_stats(WhyLagProcFaults *out, int max_count, int *out_count);

void whylag_print_report(double elapsed_sec, int is_interval);
int  whylag_export_csv(double elapsed_sec, const char *path);

int  whylag_lookup_driver(const char *name, WhyLagDriverStats *out);
void whylag_driver_advice(const char *name, char *buf, size_t buflen);
int  whylag_compare_csv(const char *baseline, const char *bad, char *report, size_t report_len);
void whylag_self_check(char *buf, size_t buflen);
void whylag_settings_load(WhyLagSettings *out);
void whylag_settings_save(const WhyLagSettings *in);
void whylag_open_folder_for_file(const char *path);
void whylag_snapshot_path(char *out, size_t outlen);
int  whylag_save_snapshot(double elapsed_sec);
int  whylag_load_snapshot(double *elapsed_out);
int  whylag_snapshot_available(void);

/* Blocking sampling loop; returns 0 on success */
int  whylag_run_loop(const WhyLagOptions *opts, double *elapsed_out);

void whylag_print_usage(void);

#endif /* WHYLAG_CORE_H */
