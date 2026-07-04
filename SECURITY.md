# Security

## What whylag does

- Starts a **kernel ETW trace session** while running (requires Administrator)
- Reads kernel latency events in **user mode** via documented Windows APIs
- Writes optional CSV exports and a local snapshot to `%AppData%\whylag\`
- Stores GUI preferences in `%AppData%\whylag\settings.ini`

## Boundaries

- **User mode only.** No kernel driver, Windows service, or persistent agent is installed.
- **Local files only.** AppData holds settings, snapshots, and exports. System drivers and registry stay untouched.
- **Offline.** No network calls or telemetry.
- **No injection.** Other processes are observed through ETW only.

## Running safely

1. **Download** from official [GitHub Releases](https://github.com/Muhib-Beekun/whylag/releases) only.
2. **Run elevated only when tracing.** ETW kernel sessions require elevation.
3. **Review CSV exports** before sharing. They contain driver names and process names from the machine that ran the sample.
4. **Verify checksums** on release artifacts (SHA256 in `SHA256SUMS.txt` on each release).

## SmartScreen and unsigned binaries

Official releases are not Authenticode-signed. SmartScreen may block or warn on first run. That is normal for unsigned open-source tools.

- **Build from source** with `build.bat` if you prefer not to click through SmartScreen.
- **Verify SHA256** from the release page, then use **More info** → **Run anyway** once.
- Self-signed certificates do not help; they still show as untrusted.

There is no legitimate free equivalent to a commercial code-signing certificate for instant publisher trust.

## Reporting vulnerabilities

Report security issues through a private GitHub security advisory or by contacting the repository owner. Keep exploitable bugs off public issues until a fix ships.

## Limitations

whylag is a latency diagnostic. It reflects ETW visibility for a short sample window. Some latency sources sit outside that window or need longer captures.
