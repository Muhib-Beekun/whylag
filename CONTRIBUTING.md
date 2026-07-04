# Contributing

Thanks for your interest in whylag.

## Build

```bat
build.bat
```

Requires MinGW GCC + windres on Windows 10+. Optional: `pip install pillow` to regenerate `whylag.ico` via `tools/make_icon.py`.

## Tests

After building:

```bat
tests\run_tests.bat
```

No admin required for compare tests.

## Pull requests

1. Keep changes focused — whylag is intentionally small.
2. Match existing C style (minimal abstractions, no unnecessary dependencies).
3. Run `build.bat` and `tests\run_tests.bat` before submitting.
4. Update `CHANGELOG.md` for user-visible changes.

## Code of conduct

Be constructive. This project exists to help people diagnose frustrating system latency.
