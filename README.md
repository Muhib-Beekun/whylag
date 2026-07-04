# whylag

**Find out why your Windows system is lagging.**

A single-binary diagnostic tool that traces kernel-level latency events and tells you which drivers are responsible. No install, no custom kernel driver — just run it elevated from a terminal.

## The problem this solves

Intermittent system lag is maddening. Audio glitches, mouse stutter, UI hitches — they come and go, and Task Manager shows nothing useful. The cause is usually buried in the kernel: a driver holding the CPU too long during an interrupt (ISR) or deferred callback (DPC), or a process triggering hard page faults that stall everything.

whylag was built while chasing exactly that — audio dropouts, mouse lag, and random stutters on a multi-GPU workstation. The goal is simple: when things feel wrong, run whylag, see which driver spiked, and know where to look.

## What it measures

| Signal | What it means |
|--------|---------------|
| **DPC latency by driver** | Deferred work scheduled by interrupts. Long DPCs block the CPU from servicing audio buffers and input. |
| **ISR latency by driver** | Immediate interrupt handlers. Long ISRs block everything at the highest priority. |
| **Hard page faults by process** | Memory fetched from disk mid-operation. Causes multi-millisecond stalls. |

## Quick start

```
whylag.exe            # 10-second sample, print report
whylag.exe 30         # 30-second sample
whylag.exe -c         # continuous until Ctrl+C
whylag.exe -c -i 10   # continuous, snapshot every 10 seconds
```

Requires **Administrator** (ETW kernel tracing needs elevation).

## Example output

```
whylag 0.1.0 — find out why your system is lagging
Sampling for 10 seconds (Ctrl+C to stop early)...

[+] 229 kernel modules loaded
[+] Tracing active

============================================================
  WHYLAG REPORT  (10.0 seconds sampled)
============================================================
  Events: 192290 total | DPC: 77720 | ISR: 5060 | PageFaults: 7

  DRIVER (DPC)                    Count   Max(us)   Avg(us)   Total%
  ------------                    -----   -------   -------   ------
  HDAudBus.sys                       85       546        18    12.1%
  Wdf01000.sys                    17759       323         0    21.3%
  ntoskrnl.exe                    59209       184         0    61.7%

  DRIVER (ISR)                    Count   Max(us)   Avg(us)   Total%
  ------------                    -----   -------   -------   ------
  dxgkrnl.sys                     4045      1483        65    98.6%
  storport.sys                    1015        50         2     1.4%

  VERDICT: [WARN] Moderate latency — may cause occasional glitches.
    Worst DPC: 546 us (HDAudBus.sys)
    Worst ISR: 1483 us (dxgkrnl.sys)
============================================================
```

## How to use it for root-cause analysis

1. **Baseline when things feel fine** — `whylag 30` and save the output.
2. **Capture during a bad period** — run `whylag -c -i 10` while audio/mouse/UI is stuttering.
3. **Compare the reports** — the driver that appears in the bad capture but not the baseline (or with much higher max times) is your suspect.
4. **Fix the driver** — update, rollback, disable a feature, or adjust power settings (see table below).

## Interpreting results

| Verdict | DPC max | ISR max | Typical impact |
|---------|---------|---------|----------------|
| **OK** | < 1000 µs | < 500 µs | Fine for real-time audio at any buffer size |
| **WARN** | < 5000 µs | < 2000 µs | May glitch at small audio buffers (< 256 samples) |
| **BAD** | > 5000 µs | > 2000 µs | Audible dropouts, mouse stutter, UI hitches |

## Common drivers and what to try

| Driver | Likely hardware | Things to try |
|--------|----------------|---------------|
| `nvlddmkm.sys` | NVIDIA GPU | Update/rollback driver; disable monitoring overlays |
| `dxgkrnl.sys` | Display / GPU | Update GPU driver; reduce connected displays |
| `HDAudBus.sys` | Audio | Update audio driver; check for IRQ conflicts |
| `tcpip.sys` / `ndis.sys` | Network | Update NIC driver; disable NIC power saving |
| `CLASSPNP.SYS` / `storport.sys` | Storage | Check disk health; update storage drivers |
| `USBPORT.sys` | USB | Disable USB selective suspend; update chipset drivers |

## How it works

whylag uses **Event Tracing for Windows (ETW)** — a documented, built-in Windows API for kernel telemetry:

1. Starts a system trace session with DPC, interrupt, and hard-fault flags
2. Consumes events in real-time with high-resolution QPC timestamps
3. Resolves routine addresses to driver names via `NtQuerySystemInformation`
4. Reports per-driver max/avg execution times and a pass/fail verdict

No custom kernel driver is installed. Everything runs in user mode using public Windows APIs.

## Building

Requires GCC (MinGW) or MSVC on Windows 8+.

```bash
gcc -O2 -o whylag.exe whylag.c -ltdh -ladvapi32
```

## Options

```
whylag [OPTIONS] [DURATION]

  DURATION               Seconds to sample (default: 10)
  -c, --continuous       Run until Ctrl+C
  -i, --interval SEC     Report interval in continuous mode (default: 5)
  -q, --quiet            Suppress progress, only show reports
  -h, --help             Show help
  -v, --version          Show version
```

## Scope

whylag is a **diagnostic tool**, not a fixer. It tells you *what* is causing latency and *which driver* is responsible. Fixing it is up to you — driver updates, hardware changes, power plan tweaks, or disabling specific features.

It does not modify your system, install services, or persist anything after exit.

## License

MIT — see [LICENSE](LICENSE).
