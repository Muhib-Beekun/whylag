# Origin — why whylag exists

whylag was built to diagnose a real machine problem, not as an abstract tooling exercise.

## The problem

After installing an **RTX 3090** (top PCIe slot) alongside an existing **RTX 4070** (bottom slot), the workstation developed severe **scroll stutter**, **audio dropouts**, **mouse lag**, and **UI hitches** that Task Manager could not explain. CPU and GPU utilization looked fine while interaction felt wrong.

## First whylag capture (2026-07-05)

20-second elevated ETW sample during general use:

| Driver | Max DPC (µs) | Max ISR (µs) | Notes |
|--------|--------------|--------------|-------|
| **dxgkrnl.sys** | 4072 | **5703** | Display/GPU stack — primary scroll path |
| **nvlddmkm.sys** | **4196** | — | NVIDIA kernel driver |
| winhvr.sys | 521 | — | Hyper-V noise (high count, lower peaks) |

**Verdict: BAD** — worst activity on **CPU 0** (typical primary display core).

**Hypothesis:** Dual-GPU topology change (3090 primary in top slot) shifted display/compositing load; NVIDIA driver + dxgkrnl ISR/DPC spikes correlate with scroll jank.

## Hardware (this machine)

| Slot | GPU | Role |
|------|-----|------|
| Top (primary) | RTX 3090 | Installed ~when stutter began; user intends to keep |
| Bottom | RTX 4070 | Was present before 3090 install |

## Constraints

- Do **not** recommend removing the 3090 as the default fix.
- **2026-07-05 diagnosis:** displays on 4070 (PCIe x1); 3090 has no active display. Prefer moving cables to 3090 before driver churn.
- Investigate: BIOS lane split, Hyper-V (`winhvr.sys`), overlay software, Meta Virtual Monitor.

## Related files

- Case log (detailed): `.temp/cases/dual-gpu-3090-4070-stutter.md`
- Sample CSV: `.temp/scroll-stutter.csv`
- Sample report: `.temp/scroll-report.txt`

## Agent instructions

When the user mentions scroll stutter, 3090/4070, dual GPU, or "why I built this", read **docs/ORIGIN.md** and `.temp/cases/dual-gpu-3090-4070-stutter.md`. Compare new CSV captures to the baseline. Focus on **nvlddmkm.sys** and **dxgkrnl.sys** max_us regressions. Run `scripts/gpu-driver-check.ps1` for driver state.
