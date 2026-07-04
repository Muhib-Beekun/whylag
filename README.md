# lagmon

A lightweight, single-binary DPC/ISR latency monitor for Windows. No installation, no unsigned drivers, no dependencies.

**lagmon** captures kernel Deferred Procedure Call (DPC) and Interrupt Service Routine (ISR) execution times in real-time using Event Tracing for Windows (ETW), attributes them to specific driver modules, and tells you exactly which drivers are causing system latency.

## Why?

The standard tool for this (LatencyMon) requires an unsigned kernel driver that modern Windows blocks with Secure Boot / HVCI. The alternative (xperf/WPA) requires installing the Windows ADK and has a steep learning curve.

lagmon needs nothing but an admin prompt. It's a single .exe you can run from anywhere.

## What it measures

| Metric | Why it matters |
|--------|----------------|
| **DPC execution time per driver** | DPCs that run too long block the CPU from servicing audio buffers, causing dropouts |
| **ISR execution time per driver** | ISRs run at the highest priority; long ISRs block everything else |
| **Hard page faults per process** | Disk fetches during playback cause multi-millisecond stalls |

## Quick start

```
lagmon.exe            # 10-second sample, print report
lagmon.exe 30         # 30-second sample
lagmon.exe -c         # continuous until Ctrl+C
lagmon.exe -c -i 10   # continuous, snapshot every 10 seconds
```

Must be run as **Administrator** (ETW kernel tracing requires elevation).

## Example output

```
lagmon 0.1.0 — DPC/ISR latency monitor
Sampling for 10 seconds (Ctrl+C to stop early)...

[+] 229 kernel modules loaded
[+] Tracing active

============================================================
  LAGMON REPORT  (10.0 seconds sampled)
============================================================
  Events: 192290 total | DPC: 77720 | ISR: 5060 | PageFaults: 7

  DRIVER (DPC)                    Count   Max(us)   Avg(us)   Total%
  ------------                    -----   -------   -------   ------
  HDAudBus.sys                       85       546        18    12.1%
  Wdf01000.sys                    17759       323         0    21.3%
  ntoskrnl.exe                    59209       184         0    61.7%
  tcpip.sys                           1        56        56     0.1%
  nvlddmkm.sys                      16        18        13     2.1%

  DRIVER (ISR)                    Count   Max(us)   Avg(us)   Total%
  ------------                    -----   -------   -------   ------
  dxgkrnl.sys                     4045      1483        65    98.6%
  storport.sys                    1015        50         2     1.4%

  VERDICT: [WARN] Moderate latency — may cause occasional glitches.
    Worst DPC: 546 us (HDAudBus.sys)
    Worst ISR: 1483 us (dxgkrnl.sys)
    Thresholds: DPC <1000/<5000/>5000 us | ISR <500/<2000/>2000 us
============================================================
```

## Interpreting results

### Verdict thresholds

| Level | DPC | ISR | Meaning |
|-------|-----|-----|---------|
| **OK** | < 1000 µs | < 500 µs | Fine for real-time audio at any buffer size |
| **WARN** | < 5000 µs | < 2000 µs | May glitch at small buffer sizes (< 256 samples) |
| **BAD** | > 5000 µs | > 2000 µs | Will cause audible dropouts, game stuttering |

### Common offenders and fixes

| Driver | What it is | Typical fix |
|--------|-----------|-------------|
| `nvlddmkm.sys` | NVIDIA GPU driver | Update driver; disable GPU monitoring overlays |
| `dxgkrnl.sys` | DirectX graphics kernel | GPU driver update; reduce display count |
| `HDAudBus.sys` | HD Audio bus | Update audio driver; check for IRQ conflicts |
| `tcpip.sys` | TCP/IP stack | Disable RSS/TCP offloading; update NIC driver |
| `CLASSPNP.SYS` | Storage class driver | Check disk health; update storage drivers |
| `ndis.sys` | Network stack | Update NIC driver; disable power management on NIC |
| `storport.sys` | Storage port driver | Check disk health; disable write caching if SSD |
| `USBPORT.sys` | USB controller | Disable USB selective suspend; update chipset drivers |
| `Wdf01000.sys` | WDF framework | Usually benign; indicates a WDF-based driver is busy |

## Building from source

Requires a C compiler (GCC/MinGW or MSVC).

```bash
# MinGW
gcc -O2 -o lagmon.exe lagmon.c -ltdh -ladvapi32

# MSVC
cl /O2 lagmon.c advapi32.lib tdh.lib
```

## How it works

1. **ETW kernel trace**: Starts a system trace session with `EVENT_TRACE_FLAG_DPC | EVENT_TRACE_FLAG_INTERRUPT | EVENT_TRACE_FLAG_MEMORY_HARD_FAULTS`
2. **Real-time consumption**: A consumer thread receives events via `ProcessTrace` in real-time mode with QPC timestamps
3. **Address resolution**: Maps DPC/ISR routine addresses to driver names using `NtQuerySystemInformation(SystemModuleInformation)` — no kernel driver needed
4. **Timing**: Each DPC/ISR completion event carries its own start timestamp; delta = event_time - initial_time

This is fundamentally the same data source that LatencyMon and xperf use (ETW NT Kernel Logger), but without requiring an unsigned driver for address resolution.

## Requirements

- Windows 8 or later (uses `EVENT_TRACE_SYSTEM_LOGGER_MODE`)
- Administrator privileges
- x64 (could be adapted for ARM64)

## Options

```
lagmon [OPTIONS] [DURATION]

  DURATION               Seconds to sample (default: 10)
  -c, --continuous       Run until Ctrl+C
  -i, --interval SEC     Report interval in continuous mode (default: 5)
  -q, --quiet            Suppress progress, only show reports
  -h, --help             Show help
  -v, --version          Show version
```

## License

MIT
