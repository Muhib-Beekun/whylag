# whylag

**Find out why your Windows system is lagging.**

A single-binary diagnostic tool that traces kernel-level latency events and tells you which drivers are responsible. No install, no custom kernel driver — run elevated from a terminal or use the GUI.

## The problem this solves

Intermittent system lag is maddening. Audio glitches, mouse stutter, UI hitches — they come and go, and Task Manager shows nothing useful. The cause is usually buried in the kernel: a driver holding the CPU too long during an interrupt (ISR) or deferred callback (DPC), or a process triggering hard page faults that stall everything.

whylag was built while chasing exactly that — audio dropouts, mouse lag, and random stutters on a multi-GPU workstation. When things feel wrong, run whylag, see which driver spiked, and know where to look.

## What it measures

| Signal | What it means |
|--------|---------------|
| **DPC latency by driver** | Deferred work scheduled by interrupts. Long DPCs block the CPU from servicing audio buffers and input. |
| **ISR latency by driver** | Immediate interrupt handlers. Long ISRs block everything at the highest priority. |
| **Per-CPU breakdown** | Which logical CPU saw the worst DPC/ISR latency during the sample. |
| **Hard page faults by process** | Memory fetched from disk mid-operation. Causes multi-millisecond stalls. |

## Quick start

**CLI** (requires Administrator):

```
whylag.exe            # 10-second sample, print report
whylag.exe 30         # 30-second sample
whylag.exe -o baseline.csv 30   # export CSV for later comparison
whylag.exe -c         # continuous until Ctrl+C
whylag.exe -c -i 10   # continuous, snapshot every 10 seconds
```

**GUI** — run `whylag-gui.exe` as Administrator:

- Set duration (or continuous), click **Start**
- Live counters while sampling; tabbed tables for DPC, ISR, per-CPU, and page faults
- **Export CSV** for baseline captures
- **Compare CSVs** — pick baseline + bad-period files to see which drivers regressed

## Root-cause workflow

1. **Baseline when things feel fine** — `whylag -o baseline.csv 30` (or GUI → Export CSV).
2. **Capture during a bad period** — `whylag -c -i 10` or GUI continuous mode while stuttering.
3. **Compare** — GUI **Compare CSVs**, or diff the CSVs manually (look for higher `max_us` or new drivers in `dpc`/`isr` rows).
4. **Fix the driver** — update, rollback, disable a feature, or adjust power settings (see table below).

## CSV format

Each export is one sample with rows like:

```csv
sample_seconds,section,name,pid,cpu,count,max_us,avg_us,total_pct
30.0,dpc,HDAudBus.sys,,,85,546,18,12.1
30.0,isr,dxgkrnl.sys,,,4045,1483,65,98.6
30.0,cpu_dpc,CPU 0,,0,12345,892,,
30.0,fault,chrome.exe,1234,,7,,,
```

Sections: `summary`, `dpc`, `isr`, `cpu_dpc`, `cpu_isr`, `fault`.

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

whylag uses **Event Tracing for Windows (ETW)** — documented, built-in kernel telemetry:

1. Starts a system trace session with DPC, interrupt, and hard-fault flags
2. Consumes events in real-time with high-resolution QPC timestamps
3. Resolves routine addresses to driver names via `NtQuerySystemInformation`
4. Reports per-driver and per-CPU max/avg execution times and a pass/fail verdict

No custom kernel driver is installed. Everything runs in user mode using public Windows APIs.

## Building

Requires GCC (MinGW) on Windows 8+.

```bat
build.bat
```

Or manually:

```bash
gcc -O2 -o whylag.exe whylag.c whylag_core.c -ltdh -ladvapi32
gcc -O2 -o whylag-gui.exe whylag_gui.c whylag_core.c -ltdh -ladvapi32 -lcomctl32 -lcomdlg32 -lgdi32 -luser32 -mwindows
```

Prebuilt binaries are attached to [GitHub Releases](https://github.com/Muhib-Beekun/whylag/releases) when tagged (`v0.2.0`, etc.).

## CLI options

```
whylag [OPTIONS] [DURATION]

  DURATION               Seconds to sample (default: 10)
  -c, --continuous       Run until Ctrl+C
  -i, --interval SEC     Report interval in continuous mode (default: 5)
  -o, --csv FILE         Export report to CSV
  -q, --quiet            Suppress progress, only show reports
  -h, --help             Show help
  -v, --version          Show version
```

## Scope

whylag is a **diagnostic tool**, not a fixer. It tells you *what* is causing latency and *which driver* is responsible. Fixing it is up to you — driver updates, hardware changes, power plan tweaks, or disabling specific features.

It does not modify your system, install services, or persist anything after exit.

## License

MIT — see [LICENSE](LICENSE).
