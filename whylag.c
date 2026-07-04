/*
 * whylag - CLI entry point
 * https://github.com/Muhib-Beekun/whylag
 *
 * MIT License — see LICENSE file.
 */

#include "whylag_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    WhyLagOptions opts = {0};
    opts.duration = 10;
    opts.interval = 5;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            whylag_print_usage(); return 0;
        }
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("whylag %s\n", WHYLAG_VERSION); return 0;
        }
        if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--continuous") == 0) {
            opts.continuous = 1; continue;
        }
        if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
            opts.quiet = 1; continue;
        }
        if ((strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--interval") == 0) && i + 1 < argc) {
            opts.interval = atoi(argv[++i]);
            if (opts.interval < 1) opts.interval = 5;
            continue;
        }
        if ((strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--csv") == 0) && i + 1 < argc) {
            opts.csv_path = argv[++i];
            continue;
        }
        int d = atoi(argv[i]);
        if (d > 0) opts.duration = d;
    }

    printf("whylag %s — find out why your system is lagging\n", WHYLAG_VERSION);
    if (opts.continuous)
        printf("Continuous mode (Ctrl+C to stop, reporting every %ds)\n\n", opts.interval);
    else
        printf("Sampling for %d seconds (Ctrl+C to stop early)...\n\n", opts.duration);

    if (whylag_start_session(opts.quiet) != 0) return 1;

    double elapsed = 0;
    whylag_run_loop(&opts, &elapsed);

    whylag_request_stop();
    whylag_stop_session();
    Sleep(300);

    whylag_print_report(elapsed, 0);
    if (opts.csv_path)
        whylag_export_csv(elapsed, opts.csv_path);

    return 0;
}
