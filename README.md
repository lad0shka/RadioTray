<p align="center">
  <img src="assets/icon.png" alt="RadioTray icon" width="128">
</p>

# RadioTray

A small internet-radio player for Windows 11 that lives in the notification area. It has no playback window. It is available as a per-user installer or as one portable EXE.

## Features

- Left-click the tray icon to play or stop.
- Right-click to choose from 20 stations in three categories.
- Hover to see the station and current artist/title when the stream supplies them.
- Station-name fallback when no track metadata is available.
- Remembers the selected station and starts silently.
- Optional start with Windows, selectable in the installer or from the menu.
- Starts playback when the connection and decoder are ready, without a fixed startup countdown.
- Downloads audio in the background into a network buffer with a capacity of up to 30 seconds.
- Embeds the audio library, so no DLL needs to sit beside the EXE.

## Usage

Run `RadioTray.exe` (or the installed copy). Windows may put the icon in the hidden-icons area of the taskbar.

| Action | Result |
| --- | --- |
| Left-click | Play / stop |
| Right-click | Station categories, Start with Windows, Exit |
| Hover | Station and current track, if available |
| Windows Volume mixer | Adjust volume |

The triangle means stopped. The square means connecting or playing. Stopping disconnects the stream; starting reconnects to the current broadcast.

Only one instance runs at a time. Application startup does not automatically start playback.

When upgrading, close the running copy before launching its replacement. RadioTray also detects a running **Anime radio** instance (its predecessor) to avoid duplicate playback.

## Start with Windows

Two equivalent ways to enable it:

- The **Start with Windows** item in the right-click menu (checked when enabled).
- The option on the installer's "Additional tasks" page.

Both write `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\RadioTray`. The entry starts RadioTray silently at sign-in; it does not begin playback. If the EXE is moved, the entry is updated the next time it runs.

## Stations

| Category | Stations |
| --- | --- |
| **J-POP / J-ROCK** | LISTEN.moe – JPOP, AnimeRadio.de, ANISON.FM, J-Pop Powerplay Kawaii, AnimeAMAZE, Gensokyo Radio, J-Rock Powerplay, J-Pop Sakura, J-Pop Powerplay |
| **CHILLSYNTH** | Nightride FM, Chillsynth FM, Datawave FM, Spacesynth FM, Darksynth FM, Horrorsynth FM, EBSM |
| **MUSIC FOR WORK** | Code Radio, Space Station Soma, Synphaera Radio, Groove Salad |

The CHILLSYNTH category groups the seven live [Nightride FM](https://nightride.fm/) channels; they cover several electronic genres. Station streams and websites are listed in [docs/STATIONS.md](docs/STATIONS.md).

Stations are operated by their respective providers. Stream availability, metadata and programming can change.

## Track information

RadioTray reads titles supplied inside the audio stream. It does not need a separate website request to retrieve each title. The tooltip updates while playback is active and clears the old title on stop or a station change.

Metadata is checked about once per second. UTF-8 titles are supported; legacy ICY text falls back to the Windows ANSI code page.

Some broadcasts omit metadata, send empty titles, or update them late. In those cases the tooltip shows the station name. Windows limits a standard tray tooltip to 127 UTF-16 code units, so long track names are shortened.

## Buffering and startup

The download buffer's **capacity** and initial preloading are separate settings. RadioTray requests minimal initial preloading, starts as soon as the stream is ready, and continues downloading as fast as the station permits.

A 30-second capacity does not mean 30 seconds are already downloaded at startup. If a station delivers no initial reserve, or a connection gap exceeds the actual downloaded reserve, playback can still pause. Connection setup and decoder initialization also take time.

## Requirements

- Windows 11, x64.
- A working audio output and internet connection.

Qt, Python, FFmpeg and a separately installed C++ runtime are not required to run the EXE or the installer.

## Installer

`installer\build.ps1` builds the release EXE and compiles `installer\RadioTray.iss` with [Inno Setup 6](https://jrsoftware.org/isinfo.php). The installer:

- installs per user into `%LOCALAPPDATA%\Programs\RadioTray` (no administrator rights);
- creates a Start menu entry, with an optional desktop shortcut;
- offers the Start with Windows option;
- closes a running copy during upgrade or uninstall;
- on uninstall, removes the autostart entry, the saved station selection and the library cache.

## Build from source

Install Visual Studio 2022 C++ Build Tools with the Windows SDK and CMake.

The official x64 BASS headers, import library, DLL and license are in `source/vendor/bass`. The DLL is embedded into the EXE at build time.

```powershell
cmake -S source -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The output is `build/Release/RadioTray.exe`. Copy only this EXE to run it. The application uses the static MSVC runtime (`/MT`).

To also build the installer, install Inno Setup 6 and run:

```powershell
powershell -ExecutionPolicy Bypass -File installer\build.ps1
```

The result is `installer\Output\RadioTray-Setup-<version>.exe`.

## Verification

The built-in verification modes use muted playback and temporary test settings:

```powershell
$exe = (Resolve-Path ".\build\Release\RadioTray.exe").Path
$testReport = Join-Path $env:TEMP "radiotray-tests.json"
Start-Process -FilePath $exe -ArgumentList ('--self-test "{0}"' -f $testReport) -Wait
Get-Content -LiteralPath $testReport

$probeReport = Join-Path $env:TEMP "radiotray-probe.json"
Start-Process -FilePath $exe -ArgumentList ('--probe-url "https://listen.moe/fallback" "{0}"' -f $probeReport) -Wait
Get-Content -LiteralPath $probeReport
```

The full self-test contacts the real stations and checks playback, menus, cancellation, resource release, persisted selection and Exit. Its result depends on network and station availability. The commands wait for each process to finish before displaying its report. If you choose a custom report location, its parent directory must already exist. The results of the last run are in [docs/VERIFICATION.md](docs/VERIFICATION.md).

## Settings and library cache

- Selected station: `HKCU\Software\RadioTray`.
- Start with Windows: `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`, value `RadioTray`.
- Embedded-library cache: `%LOCALAPPDATA%\RadioTray\runtime\<library-sha256>\bass.dll`.

The cache is created automatically without administrator access. Before loading, RadioTray compares the cached file with the embedded bytes. A damaged copy can be restored when the cache is writable and unlocked. The library is loaded by its verified absolute path.

The previous **Anime radio** station selection is migrated automatically. Existing legacy settings are retained.

## Source layout

| Path | Purpose |
| --- | --- |
| `source/main.cpp` | Tray UI, station catalogue, playback state, autostart and settings |
| `source/stream_metadata.cpp` | Stream tags and tooltip text |
| `source/embedded_bass.cpp` | Embedded-library extraction, verification and loading |
| `source/app.rc`, `source/app.manifest` | Icon, version information, embedded DLL resource, manifest |
| `source/version.h` | Application version |
| `source/app.ico` | Application icon (EXE, installer) |
| `source/vendor/bass` | Unmodified third-party build dependencies and license |
| `installer/RadioTray.iss`, `installer/build.ps1` | Inno Setup installer script and build script |
| `assets/icon.png` | Icon used in this README |
| `docs/` | Station list and verification notes |

## Third-party licensing

Audio playback uses [BASS](https://www.un4seen.com/bass.html) by Un4seen Developments. BASS is free for non-commercial use under its own terms; commercial use requires an appropriate BASS license. See [the included BASS license](BASS-LICENSE.txt).

Radio streams and station names belong to their respective operators. This project does not redistribute or own their broadcasts.

## Reporting a problem

Include the Windows version, station name, whether the problem concerns connection, audio or metadata, and steps to reproduce it. For playback issues, attach a probe report if available. Do not include credentials or private stream URLs.
