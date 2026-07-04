# whylag

**Find out why your Windows system is lagging.**

whylag is a lightweight Windows diagnostic that uses built-in kernel tracing (ETW) to show which drivers and processes are causing latency spikes. No installer, no kernel driver of its own—just run elevated and read the report.

Built while chasing real-world stutters: audio dropouts, mouse lag, and UI hitches that Task Manager never explains. When something feels wrong, whylag tells you *which* `.sys` file held the CPU too long.

## What you get

Two programs, one engine:

| Binary | Role |
|--------|------|
| **`whylag-gui.exe`** | Dark-themed GUI — live tables, verdict, CSV export/compare, double-click row details |
| **`whylag.exe`** | CLI for scripts, SSH, and quick terminal checks |

Both require **Administrator** (ETW kernel sessions need elevation).

## The problem this solves

Intermittent lag is maddening because the cause hides in the kernel. A driver runs too long in an interrupt handler (ISR) or deferred callback (DPC), or a process triggers hard page faults that stall the machine for milliseconds. whylag surfaces that in seconds instead of guessing from Task Manager CPU graphs.

## What it measures

| Signal | What it means |
|--------|---------------|
| **DPC latency by driver** | Deferred interrupt work. Long DPCs block audio buffers, input, and the UI thread. |
| **ISR latency by driver** | Immediate interrupt handlers at highest priority. Should stay very short. |
| **Per-CPU breakdown** | Which logical CPU saw the worst DPC/ISR during the sample (IRQ affinity hints). |
| **Hard page faults by process** | Memory pulled from disk mid-operation — multi-ms stalls. |
| **Context switches** | Aggregate count during the sample — high activity can correlate with scheduler pressure. |
| **Disk I/O events** | Kernel disk activity count — useful context when faults or storage drivers spike. |

Driver names are resolved from kernel module maps, PSAPI, and ETW image-load events (not a wall of `(unknown)`).

## Quick start

**GUI** (recommended):

1. Run `whylag-gui.exe` as Administrator.
2. Set **Duration** (default 10 s) or check **Continuous** to capture a bad period.
3. Click **Start** — watch live counters and tabbed results (DPC, ISR, Per-CPU, Page faults).
4. **Double-click a row** for detail and driver-specific fix suggestions.
5. **Export CSV** when fine; capture again when stuttering; **Compare** the two files.

**CLI**:

```bat
whylag.exe                  rem 10-second sample, print report
whylag.exe 30               rem 30-second sample
whylag.exe -o baseline.csv 30
whylag.exe -c               rem continuous until Ctrl+C
whylag.exe -c -i 10         rem continuous, snapshot every 10 s
whylag compare good.csv bad.csv   rem diff max latency between exports
```

Press **F1** or **Help** in the GUI for the full in-app guide.

## Root-cause workflow

1. **Baseline** when the system feels fine — export CSV (`whylag -o baseline.csv 30` or GUI).
2. **Capture** during stutter — continuous mode while it happens.
3. **Compare** — GUI **Compare CSVs**, CLI `whylag compare`, or diff `max_us` in `dpc`/`isr` rows.
4. **Fix** — update/rollback the flagged driver, adjust power settings, disable overlays, etc.

Focus on **Max (us)** — the worst single event in the window. That is what causes audible glitches and mouse stutter.

## Verdict thresholds

| Verdict | DPC max | ISR max | Typical impact |
|---------|---------|---------|----------------|
| **OK** | < 1000 µs | < 500 µs | Fine for real-time audio at any buffer size |
| **WARN** | < 5000 µs | < 2000 µs | May glitch at small audio buffers (< 256 samples) |
| **BAD** | > 5000 µs | > 2000 µs | Audible dropouts, mouse stutter, UI hitches |

## CSV format

```csv
sample_seconds,section,name,pid,cpu,count,max_us,avg_us,total_pct
30.0,summary_dpc,,,,12345,,,
30.0,summary_cswitch,,,,890012,,,
30.0,summary_disk,,,,45678,,,
30.0,dpc,nvlddmkm.sys,,,85,546,18,12.1
30.0,isr,dxgkrnl.sys,,,4045,1483,65,98.6
30.0,cpu_dpc,CPU 0,,0,12345,892,,
30.0,fault,chrome.exe,1234,,7,,,
```

Sections: `summary_*`, `dpc`, `isr`, `cpu_dpc`, `cpu_isr`, `fault`.

## Common drivers and what to try

| Driver | Likely hardware | Things to try |
|--------|-----------------|---------------|
| `nvlddmkm.sys` | NVIDIA GPU | Update/rollback driver; disable monitoring overlays |
| `dxgkrnl.sys` | Display / GPU | Update GPU driver; reduce connected displays |
| `HDAudBus.sys` | Audio | Update audio driver; check IRQ conflicts |
| `tcpip.sys` / `ndis.sys` | Network | Update NIC driver; disable NIC power saving |
| `storport.sys` / `stornvme.sys` | Storage | Check disk health/SMART; update firmware |
| `winhvr.sys` / `vmbusr.sys` | Hyper-V / VMs | Pause VMs or reduce hypervisor load |
| `USBPORT.sys` | USB | Disable USB selective suspend; update chipset |

Double-click a driver row in the GUI for tailored advice.

## How it works

whylag uses **Event Tracing for Windows (ETW)** — documented, built-in telemetry:

1. Starts a kernel trace session (DPC, interrupt, image load, context switch, disk I/O, hard faults).
2. Consumes events in real time with QPC timestamps.
3. Resolves ISR/DPC routine addresses to driver names via `NtQuerySystemInformation`, PSAPI, and ETW image loads.
4. Aggregates per-driver, per-CPU, and per-process stats; prints a verdict.

Everything runs in user mode. Nothing is installed or left running after exit. Settings (duration, export folder) are stored in `%AppData%\whylag\settings.ini`.

## Building

Requires **GCC (MinGW)** and **windres** on Windows 10+.

```bat
build.bat
```

`build.bat` stops any running copy, compiles both binaries, and links the app icon from `whylag.rc`. If an elevated GUI is still open, the fresh build lands in `.temp\build\whylag-gui.exe` until you close the old one.

Regenerate the icon (optional, needs `pip install pillow`):

```bat
python tools\make_icon.py
```

Manual build:

```bat
gcc -O2 -Wall -o whylag.exe whylag.c whylag_core.c -ltdh -ladvapi32 -lshell32
windres whylag.rc -O coff -o whylag_res.o
gcc -O2 -Wall -o whylag-gui.exe whylag_gui.c whylag_gui_theme.c whylag_help.c whylag_detail.c whylag_core.c whylag_res.o -ltdh -ladvapi32 -lcomctl32 -lcomdlg32 -lgdi32 -luser32 -ldwmapi -luxtheme -lshell32 -mwindows
```

## CLI options

```
whylag [OPTIONS] [DURATION]
whylag compare BASELINE.csv BAD.csv

  DURATION               Seconds to sample (default: 10)
  -c, --continuous       Run until Ctrl+C
  -i, --interval SEC     Report interval in continuous mode (default: 5)
  -o, --csv FILE         Export report to CSV
  -q, --quiet            Suppress progress, only show reports
  -h, --help             Show help
  -v, --version          Show version
```

## Scope and limits

whylag is a **diagnostic tool**, not a fixer. It shows *what* spiked and *which driver* to investigate. It does not modify drivers, install services, or persist tracing after exit.

It traces what ETW exposes in a short sample — not a full WPA/xperf session. For deep dives, export CSV and compare baseline vs bad periods, or capture a longer continuous sample.

## Releases

Prebuilt binaries are attached to [GitHub Releases](https://github.com/Muhib-Beekun/whylag/releases) when tagged (`v0.3.0`, etc.). CI builds on push to `main`/`master`.

The repo is currently private; a public release will ship signed binaries and a short security/limitations section in release notes.

## License

MIT — see [LICENSE](LICENSE).
