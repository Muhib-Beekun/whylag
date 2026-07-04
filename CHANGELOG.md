# Changelog

All notable changes to whylag are documented here.

## [0.3.2] - 2026-07-04

### Added
- Repository cover image (`docs/whylag-cover.jpg`) in README header
- Shared `scripts/build-binaries.bat` used by local build and CI
- GUI UAC manifest (`requireAdministrator` on launch; CLI unchanged)

### Changed
- README and SECURITY.md: SmartScreen guidance, build-from-source trust path
- Release notes mention GUI UAC-on-launch and unsigned binary expectations

## [0.3.1] - 2026-07-04

### Added
- Windows `VERSIONINFO` resource (version shown in File Properties)
- Release SHA256 checksums, LICENSE, and zip bundle on GitHub Releases
- GUI window title shows version (`whylag 0.3.1`)

### Changed
- CLI user-facing text uses ASCII hyphens (no em dash mojibake in `cmd.exe`)
- Release workflow runs regression tests before publishing
- CLI binary links app icon and version resource (same as GUI)

## [0.3.0] - 2026-07-04

### Added
- **whylag-gui.exe**: dark-themed GUI with live DPC/ISR/CPU/fault tables
- Double-click row for driver detail and fix suggestions
- CSV export, compare (GUI scrollable dialog + CLI `whylag compare`)
- Settings dialog (Opts): live refresh interval, open folder on export
- Automatic last-sample snapshot in `%AppData%\whylag\last_sample.csv` (restored on startup)
- Extra ETW counters: context switches and disk I/O (GUI, CLI, CSV)
- In-app Help (F1), tooltips, app icon
- Shared `whylag_core` engine for CLI and GUI
- CI build workflow and release-on-tag workflow
- Regression tests for CSV compare (`tests/run_tests.bat`)
- Robust CSV parser (handles empty pid/cpu columns from exports)

### Changed
- Driver name resolution fixed (ISR opcode 67, DPC 66/68/69)
- Build script kills stale processes; links via `.temp/build/` when exe is locked
- README expanded for public release

## [0.2.0] - 2026-07-03

### Added
- Initial GUI, CSV export, per-CPU stats, GitHub Actions CI

## [0.1.0] - 2026-07-03

### Added
- CLI ETW kernel latency diagnostic
