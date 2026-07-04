# Agent guide — whylag

Instructions for AI agents helping users with Windows latency on this repository.

## Project summary

**whylag** samples kernel ETW events and reports which drivers/processes caused the worst DPC/ISR latency and hard page faults. User-mode only; no custom kernel driver.

## What you can do without elevation

| Action | Command |
|--------|---------|
| Version | `whylag.exe -v` |
| Help | `whylag.exe -h` |
| Compare two exports | `whylag compare baseline.csv bad.csv` |

Use compare output to identify drivers whose `max_us` increased between a good and bad period.

## What requires Administrator

- `whylag.exe` sampling (any duration)
- `whylag-gui.exe` (manifest requests elevation on launch)

Do not promise unattended live traces unless the user confirms an elevated session.

## Recommended user workflow

1. Baseline CSV when system feels fine (GUI Export or `whylag -o baseline.csv 30` elevated).
2. Capture during stutter (continuous mode).
3. Compare exports; focus on **Max (us)** per driver in `dpc` and `isr` sections.
4. Suggest driver update/rollback for flagged `.sys` modules (see GUI detail text or README table).

## CSV format

Header:

```csv
sample_seconds,section,name,pid,cpu,count,max_us,avg_us,total_pct
```

Regression signal: higher `max_us` for the same `name` in section `dpc` or `isr`.

## MCP (planned)

A future MCP server should expose compare/parse tools only. Live ETW is out of scope for typical agent loops.

## Do not

- Claim code signing or SmartScreen trust (releases are unsigned).
- Suggest disabling security software or random registry hacks based on a short sample alone.
- Share user CSV exports publicly without warning they contain process/driver names.

## Links

- Full docs: README.md
- Machine-readable summary: llms.txt
- Security boundaries: SECURITY.md
