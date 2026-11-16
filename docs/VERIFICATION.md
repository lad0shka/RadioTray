# Verification

Checked on 16 November 2026 with RadioTray 1.1.0.0 on Windows 11 x64.

## Build and self-test

- Release build with CMake and the static MSVC runtime (`/MT`), including the embedded application icon.
- Built-in self-test (`--self-test`): 48 of 48 checks passed. It covered startup, the grouped menu with its checked selection and the Start with Windows item, toggling, cancellation, playback on all 20 stations, stream release, the icon resources, persisted selection and Exit.
- Self-test results depend on network and station availability; a rerun may differ if a station is offline.

## Installer

- `installer/build.ps1` builds the EXE and compiles `RadioTray-Setup-1.1.0.exe` with Inno Setup 6.
- Silent install into a temporary directory with the autostart task selected: files installed, `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\RadioTray` written with the installed EXE path.
- Silent uninstall: files removed and the Run value removed.

## Not checked

- The Start with Windows menu item was not clicked in an interactive session; its registry logic was exercised through the installer path only.
- The installer's Start menu, desktop shortcut and upgrade-with-running-copy behaviour were not tested interactively.
- The probe mode (`--probe-url`) was not rerun for this release.
