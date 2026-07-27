# Luna Capture 1.0.0.1

A follow-up to the first release, focused on **performance on weaker CPUs**, a proper home for video
settings, and fixing several things that plainly did not work.

Upgrading is safe: your settings, plugins and captures are kept. The installer closes a running copy
before replacing files.

---

## Downloads

| File | What it is |
| --- | --- |
| **`LunaSetup.exe`** | Installer. Checks your system, downloads the release, sets up shortcuts, autostart and the `.luna` file association. Also the uninstaller (`--uninstall`). |
| **`LunaCapture-portable.zip`** | Portable build — the app, offline SDK docs and the license. Unzip and run. |

---

## New: choose what does the work

Recording used to always take the same route through the machine, and on a CPU with a small cache
that costs real frames in game. There is now a **Processing load** setting with three modes, applied
to normal recordings *and* Instant Replay:

- **Balanced** — the previous behaviour: hardware encoder, GPU resize when downscaling.
- **Prefer GPU** — least CPU cost. Hardware encoder forced, resize kept on the GPU.
- **Prefer CPU** — software encoder and CPU resize, leaving the GPU entirely to the game. For
  machines that are GPU-limited rather than CPU-limited.

> **If a recording costs you frames, also try lowering Resolution below your monitor's native
> height.** That switches the resize onto the GPU *and* shrinks both the frame readback and the
> encode — on a weak CPU it is the single biggest one-click win, bigger than the mode above.

## New: a Video & encoder page

Recording settings were scattered across the Record page while Instant Replay had none of its own,
even though both produce the same kind of file. There is now an **Encoder** entry in the sidebar
holding everything that shapes a video, applying to recordings and Replay clips alike:

| Setting | What it does |
| --- | --- |
| Processing load | CPU / GPU / Balanced, as above |
| Codec | H.264, HEVC or AV1 |
| **Rate control** | CBR, VBR, or constant quality |
| **Quality level** | 1–100 target for constant-quality mode |
| **Encoder effort** | Speed / Balanced / Quality — better picture per bit for more encoder cost |
| Bitrate | Target for the selected codec |
| Frame rate | Applies to recordings and Replay |
| Resolution | Native, or a fixed height |
| **Keyframe interval** | Auto, or 1–10 s — shorter makes clips easier to cut |
| Cursor | Draw the pointer into the video |
| Audio bitrate | AAC, 48 kHz stereo |
| Replay length | How far the rolling buffer reaches back |

The new encoder controls go through `ICodecAPI`. Hardware encoders differ in what they implement, so
a property a driver rejects simply leaves that encoder's own default in place rather than failing the
recording. Settings are locked while a recording or Replay buffer is live, and the page says so.

---

## Fixes

**Region screenshots were blurry while Instant Replay was running.** With a recording height below
native, the recorder's CPU-side frame is downscaled for the encoder — and screenshots were being
taken from *that* frame and stretched back up. Both the region editor's frozen preview and the saved
file now come from a full-resolution snapshot. Since a replay buffer that resumes on launch is common,
this affected most region captures.

**Explorer showed captures in a seemingly random order.** The file names were always sortable
(`YYYY-MM-DD_HH-MM-SS_mmm_…`), but the folders sit under Pictures/Videos and inherit those library
templates, which sort by *Date taken* — metadata screen captures never carry. The capture folders are
now pinned to Explorer's generic template, so they sort by name, which is date order.
*If you already opened those folders, Windows remembers the old view: sort by Name once.*

**Opening a `.luna` plugin did nothing.** It relied on handing the file to a running copy, which
failed silently when the window was hidden in the tray, and was blocked outright by Windows when the
app ran elevated. Opening a plugin now shows its own small installer — plugin name, author,
description and the exact permissions it requests — and installs it directly, working the same
whether Luna is closed, in the tray, or elevated.

**Notification statuses were misleading.** Failures such as "Recorder is busy" or "Replay buffer is
off" were shown with a green success tick. Toasts now have three states: green check, amber warning
triangle, red error cross.

**Update checking barely worked.** It ran at most once every 20 hours and only at launch, so a
release published after startup stayed invisible. It now checks on launch and every 6 hours, shows a
persistent banner when an update exists, and Settings has a **Check for updates** button that always
answers — including a "Check again" that works while an update banner is already up.

**Autostart and "always run as administrator" can now be enabled together.** A Startup shortcut
cannot do both — Windows silently skips it, because the sign-in launcher never shows a UAC prompt.
Selecting both now registers a logon task with the highest available privileges instead. Creating
that task needs an elevated installer; if it is not, the installer falls back to a plain shortcut and
says so.

**Video fullscreen is now real fullscreen.** The player used to expand only inside the app window.
It now covers the whole monitor, with a floating **Exit fullscreen** button plus <kbd>Esc</kbd>,
<kbd>F</kbd> and double-click.

**Message boxes replaced.** Every stock Windows dialog is gone; deleting a file now shows a themed
confirmation with the file name, size and date, and a red destructive button.

**The window opens above other windows from the tray** — and refuses to open at all over a game in
true exclusive fullscreen, telling you to Alt-Tab first, rather than opening invisibly behind it and
stealing focus. The title bar now has only a close button; minimising did the same thing as closing,
since the window has no taskbar button.

---

## For plugin authors

`plugins/` ships five ready-made LunaScript plugins, including a **translation base** containing
every user-facing string in the app (733 entries) as a starting point for any language. All of them
are parsed by the test suite, so they cannot silently rot.

## For contributors

- The version now lives in exactly one place — the `VERSION` file at the repository root — and
  propagates to the C++ build, both executables' file properties, the build scripts and the website.
- `prepare_github_upload.bat` stages a clean, minimal source folder for uploading through the GitHub
  website, which accepts at most 100 files per drag.
- A fix worth knowing about: `resources.rc` never defined `VS_VERSION_INFO`, so every previous build
  shipped with **no readable version in its file properties**. Both executables now carry it.

---

## Known limitations

Unchanged from 1.0.0.0 and worth reading before filing a bug:

**Games in *true* exclusive fullscreen are a platform limitation.** Windows stops compositing that
monitor's desktop image, so Desktop Duplication has nothing to hand over — recording can freeze on
the last frame from before the transition, and overlays cannot be drawn on top at all.
**Workaround:** set the game to *Borderless* / *Windowed Fullscreen*. Modern flip-model fullscreen is
unaffected.

**Replay previews can appear black on a multi-GPU setup**, when a monitor is driven by a different
adapter than the capture device. Recording itself is unaffected.

**No code signing yet.** SmartScreen and some antivirus products warn on first run, and Defender's
machine-learning bucket (`Wacatac.B!ml` and similar) is a known false positive for unsigned capture
tools. The always-on low-level keyboard hook — needed so hotkeys keep working inside fullscreen
games — is the main behavioural trigger. Signing is planned.

---

## License

Luna Capture is **source-available, not open source**. You may read, compile and privately modify it,
and use it commercially — but selling it, or distributing modified builds or modified source, is not
permitted. See [`license.txt`](license.txt).

Copyright © 2026 ggrolton123. All rights reserved.
