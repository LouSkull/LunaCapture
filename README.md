<div align="center">

# Luna Capture

**Screen capture that stays out of your way.**

A lightweight **native Windows** screenshot, screen-recording, and instant-replay utility written in C++20.
No Electron, no Node.js, no browser runtime — just DirectX, WASAPI and Media Foundation talking straight to the OS.

[![Platform](https://img.shields.io/badge/platform-Windows%2010%20%7C%2011-0a7bbb)](https://github.com/LouSkull/LunaCapture)
[![Language](https://img.shields.io/badge/C%2B%2B-20-00599C)](https://github.com/LouSkull/LunaCapture)
[![Build](https://img.shields.io/badge/build-CMake%20%2B%20MSVC-064F8C)](https://github.com/LouSkull/LunaCapture)
[![License](https://img.shields.io/badge/license-Source--Available-f4b64c)](license.txt)

[**Download**](https://github.com/LouSkull/LunaCapture/releases) · [SDK Docs](website/sdk.html) · [License](license.txt)

</div>

---

## Why

Most capture tools ship a browser engine to draw a few buttons. Luna Capture is a single self-contained
`LunaCapture.exe`: the entire interface is anti-aliased GDI+, capture runs on DXGI Desktop Duplication,
and encoding goes through hardware Media Foundation encoders when your GPU has them.

No installer service. No background updater daemon. No telemetry. It idles in the tray and gets out of the way.

## Highlights

- **Screenshots** — active display, all displays stitched into one image, a precise region, or a picked window.
- **Region editor** — freezes the whole desktop for pixel-accurate selection, a move/resize tool for
  reframing what you drew, then annotate with arrow, rectangle, pen, highlighter and eraser before
  copying or saving.
- **Screen recording** — twelve codecs across MP4, fragmented MP4, WMV, AVI and 3GP (H.264, HEVC, AV1,
  VP9, MPEG-4, VC-1, MJPEG and more), FPS presets from 24 to 240, automatic foreground-game monitor
  selection, and recovery that survives fullscreen transitions and GPU resets without dropping the session.
- **Instant replay** — a rolling *encoded* buffer (not raw frames in RAM) on up to 3 monitors simultaneously.
  One hotkey writes the last moments to disk and the buffer restarts at 0:00 without interrupting capture.
- **Audio mixer** — system loopback, per-application audio, and up to 4 microphones mixed live to
  48 kHz stereo AAC, each with independent 0–200 % gain and a soft limiter.
- **LunaScript plugins** — a sandboxed, permission-checked extension language. No DLLs, no native code
  execution, no network or filesystem access. Includes a full Theme Studio.
- **One settings file** — every setting saves itself to a single portable `settings.ini`; copy it to
  another machine and the whole setup comes with it.

> **Looking for screenshots?** The project site under [`website/`](website/) renders a full visual tour —
> open [`website/index.html`](website/index.html) in a browser. It is completely offline and makes no network calls.

## Getting started

Two ways to get it, both from the [releases page](https://github.com/LouSkull/LunaCapture/releases):

- **`LunaSetup.exe`** — the installer. It checks the machine, downloads the release, and wires up
  shortcuts, autostart and the `.luna` file association for you.
- **`LunaCapture.exe`** — the portable build. Nothing to install; just run it.

To build either yourself, see [Building from source](#building-from-source).

### The installer

`LunaSetup.exe` is a single self-contained executable — no bundled payload, no dependencies of its own.

- **System check** — verifies Windows 10 1903+, a working Direct3D 11 device, and Media Foundation.
  That last one is genuinely absent on Windows **N/KN** editions until the Media Feature Pack is
  installed, and the installer says so instead of letting recording fail later.
- **What it downloads** — `LunaCapture-portable.zip` from the chosen release, expanded into the
  install folder (app, offline SDK docs, license). `build_release.bat` produces exactly that archive
  in `dist\`; attach it to the GitHub release next to `LunaSetup.exe`.
- **Pinned version** — [`installer/installer.ini`](installer/installer.ini) decides which release to
  fetch (`TargetVersion=1.0.0.1`, or `latest`). If a newer release exists than the pinned one, the
  first page shows it and offers a one-click switch — it never upgrades behind your back.
- **Upgrading in place** — if Luna Capture is running, the installer asks it to close before
  replacing files, so an upgrade never fails on a locked executable.
- **Options** — install folder, desktop and Start-menu shortcuts, start with Windows, always run as
  administrator, and the `.luna` file association. Autostart and run-as-admin are mutually exclusive,
  because a RUNASADMIN shim silently blocks Explorer's sign-in launch.
- **`.luna` association** — double-clicking a plugin hands it to Luna Capture, which copies it into
  the plugin folder, reloads, and opens the Plugins page so you can review its permissions. If the app
  is already running, the file is forwarded to that instance instead of starting a second copy.
- **Uninstall** — registered in *Apps & features*, or run `LunaSetup.exe --uninstall`. It removes the
  program, shortcuts, autostart entry, the file association and the compatibility shim, and asks
  whether to keep your settings, plugins and captures.

Everything installs **per-user** under `%LOCALAPPDATA%`, so no elevation prompt is needed.

## Default shortcuts

Every binding is rebindable in **Settings**. Hotkeys are routed through three deduplicated input paths
(Raw Input, a low-level keyboard hook, and `RegisterHotKey`) so they keep firing inside fullscreen games.

| Shortcut | Action |
| --- | --- |
| <kbd>Print Screen</kbd> | Capture the display containing the foreground window |
| <kbd>Ctrl</kbd> + <kbd>Print Screen</kbd> | Capture every display, stitched into one image |
| <kbd>Shift</kbd> + <kbd>Print Screen</kbd> | Open the full-desktop region editor |
| <kbd>Alt</kbd> + <kbd>Print Screen</kbd> | Pick a single window to capture |
| <kbd>F9</kbd> | Start / stop screen recording |
| <kbd>F10</kbd> | Save the current instant replay |
| <kbd>Alt</kbd> + <kbd>X</kbd> | Toggle the Quick Panel over fullscreen games |

## Plugins — LunaScript

A plugin is a single UTF-8 `.luna` file interpreted by the host. It is **data, not code**: it cannot call
Win32, inspect frames, open arbitrary paths, reach the network, or allocate native memory. Every declared
permission is validated before load, and each execution chain runs on a bounded instruction budget.

```
luna 1;

plugin "studio.acme.quick-capture" {
    name        "Quick Capture";
    version     "1.0.0";
    author      "Acme Studio";
    description "Captures the active display from Quick Panel.";
    requires host_api 5;

    permission capture;
    permission notifications;

    command "capture-now" {
        title "Capture now";
        description "Capture the display containing the foreground window.";
        icon "monitor";
        surface quick_panel;
        order 20;
        run {
            capture active;
            notify "Capture queued" "Luna will report when the image is saved.";
        }
    }
}
```

Drop the file into `%LOCALAPPDATA%\Luna Capture\Plugins\` and hit **Reload** on the Plugins page.
The complete language reference, Host API, and Theme Studio documentation live in
[`website/sdk.html`](website/sdk.html) — the same document ships offline with the app and opens from the
**SDK** button inside the Plugins page.

### Ready-made plugins

[`plugins/`](plugins/) holds five working starting points, each parsed by the test suite so they stay valid:

| Plugin | What it gives you |
| --- | --- |
| [`translation-base.luna`](plugins/translation-base.luna) | **Every user-facing string in the app** (724 entries) as a fill-in-the-blank locale — the base for translating Luna into any language |
| [`quick-panel-essentials.luna`](plugins/quick-panel-essentials.luna) | Region capture, replay save, recording toggle and folder shortcuts on the Quick Panel and tray |
| [`session-watchdog.luna`](plugins/session-watchdog.luna) | Turns quiet recording/Replay recovery and failure events into overlay toasts |
| [`capture-presets.luna`](plugins/capture-presets.luna) | One-press setting bundles for sharing, archiving, and fast screenshots |
| [`theme-starter.luna`](plugins/theme-starter.luna) | The full semantic colour set, filled in and ready to recolour |

**Want Luna in your language?** Start from `translation-base.luna` — see
[`plugins/README.md`](plugins/README.md) for the walkthrough.

## Building from source

**Requirements**

- Windows 10 or 11 (x64)
- CMake 3.24 or newer
- MSVC x64 toolchain — *Desktop development with C++* plus a Windows 10/11 SDK

**One-shot build and launch**

```bat
install_and_run.bat
```

This verifies the toolchain, configures CMake, builds a static-runtime Release executable, runs the
test suite, and starts `dist\LunaCapture.exe`.

**Manual build**

```bat
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

The executables land in `build\bin\` — `LunaCapture.exe` and `LunaSetup.exe`.

If something goes wrong, `diagnose.bat` prints the detected toolchain, CMake version, and build state.

### Tests

`ctest` runs three suites by default:

| Suite | Covers |
| --- | --- |
| `LunaShotCoreTests` | Core utilities, image scaling, LunaScript parsing, localization |
| `LunaShotPluginDispatchTests` | Plugin event dispatch fairness and instruction budgets |
| `LunaShotPluginExampleTests` | Every bundled plugin in `plugins/` still parses |
| `LunaShotThemeTests` | Theme renderer and backdrop patterns |

Two hardware-dependent smoke tests are excluded from the default build because they capture and encode
real frames. Build them explicitly when you touch the recording pipeline:

```bat
cmake --build build --target LunaShotRecordingSmoke
cmake --build build --target LunaShotMediaPlayerSmoke
```

### Publishing the sources by hand

`git push` is the normal way to update the repository. If you upload through the GitHub website
instead, its uploader takes at most 100 files per drag — so run:

```bat
prepare_github_upload.bat
```

It stages a clean `github-source\` folder containing only what belongs in the repository (no build
output, no `dist`, no captures), prints the file count per folder, and tells you whether it fits in
one upload or needs splitting.

### Versioning

The version lives in exactly one place: the [`VERSION`](VERSION) file at the repository root. CMake reads it
into `project(VERSION)` and propagates it to the C++ `kAppVersion`, the executable's version resource, the
build scripts, and the website badges. **Bump that one file for a release** — nothing else needs editing.

## Repository layout

```
src/           Implementation (capture, recording, audio, UI, plugins)
include/       Public headers for the above
installer/     Standalone installer / uninstaller (LunaSetup.exe)
tests/         ctest suites and hardware smoke tests
plugins/       Ready-made LunaScript plugins, incl. the full translation base
assets/        Icon, capture sound, application manifest
website/       Project site — overview, SDK docs, license
docs/          Offline SDK copy shipped next to the executable
cmake/         Generated-header templates
VERSION        Single source of truth for the app version
```

## Where Luna Capture keeps things

| Path | Contents |
| --- | --- |
| `%USERPROFILE%\Pictures\Luna Capture` | Screenshots (default, configurable) |
| `%USERPROFILE%\Videos\Luna Capture` | Recordings and saved replays (default, configurable) |
| `%LOCALAPPDATA%\Luna Capture\settings.ini` | All settings and hotkey bindings |
| `%LOCALAPPDATA%\Luna Capture\Plugins\` | Installed `.luna` plugins |
| `%LOCALAPPDATA%\Luna Capture\capture.log` | Diagnostic log |

Uninstalling is deleting the executable; removing the `%LOCALAPPDATA%\Luna Capture` folder clears
everything else.

## Privacy

Luna Capture has no telemetry, no analytics, and no accounts. The only network request it ever makes is an
update check against the GitHub releases API, which you can trigger or ignore from **Settings → Updates**.
Plugins cannot make network requests at all.

## License

Luna Capture is **source-available, not open source**. You may read, compile, and privately modify it, and
use it commercially — but selling it, or distributing modified builds or modified source, is not permitted.

See [`license.txt`](license.txt) for the authoritative terms, or the
[rendered version](website/license.html) for a plain-language summary alongside the full text.

Copyright © 2026 ggrolton123. All rights reserved.
