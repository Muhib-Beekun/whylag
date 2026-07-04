# Security

## What whylag does

- Starts a **kernel ETW trace session** while running (requires Administrator)
- Reads kernel latency events in **user mode** via documented Windows APIs
- Writes optional CSV exports and a local snapshot to `%AppData%\whylag\`
- Stores GUI preferences in `%AppData%\whylag\settings.ini`

## What whylag does not do

- Does not install a kernel driver, service, or persistent agent
- Does not modify system settings, drivers, or registry (except its own AppData files)
- Does not send data over the network
- Does not inject code into other processes

## Running safely

1. **Download** from official [GitHub Releases](https://github.com/Muhib-Beekun/whylag/releases) only.
2. **Run elevated only when tracing** — elevation is required for ETW kernel sessions.
3. **Review CSV exports** before sharing — they contain driver names and process names from your machine.
4. **Verify checksums** on release artifacts when published (SHA256 in release notes).

## Reporting vulnerabilities

If you find a security issue, open a private security advisory on GitHub or contact the repository owner. Do not file public issues for exploitable bugs until patched.

## Limitations

whylag is a diagnostic tool, not a security product. It reflects ETW visibility for a short sample window and may not capture every source of system latency.
