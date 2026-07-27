# Luna Capture 1.0.0.1

A follow-up to the first public release, focused on **performance on weaker CPUs**, a single home for
every video setting, a much wider choice of codecs, and fixing a long list of things that plainly did
not work — several of them on multi-monitor setups.

Upgrading is safe for your captures, settings and plugins. The installer closes a running copy before
replacing files. **One feature was removed** — see [Removed: Profiles](#removed-profiles) below.

---

## Downloads

| File | What it is |
| --- | --- |
| **`LunaSetup.exe`** | Installer. Checks your system, downloads the release, sets up shortcuts, autostart and the `.luna` file association. It also installs itself as `LunaUninstall.exe` next to the app, so uninstalling works even after this download is deleted. |
| **`LunaCapture-portable.zip`** | Portable build — the app, offline SDK docs and the license. Unzip and run. |

---

## Removed: Profiles

The **Profiles** page is gone, along with the five built-in presets and any custom profiles you saved.

It existed to answer "switch my whole setup in one click," but it duplicated the settings system: the
same values lived both in the profile files and in the live configuration, and the two drifted apart
constantly — the page spent most of its space telling you a profile was *MODIFIED*.

**What replaces it:**

- **Every setting already saves itself, immediately, to one file.** That file — `settings.ini` in
  `%LocalAppData%\Luna Capture` — *is* your portable configuration. Copy it to another machine and
  the whole setup comes with it. Nothing to export, nothing to remember to save.
- **For switching between jobs in one press, use the `capture-presets` plugin** that ships in
  `plugins/`. It applies a complete bundle — resolution, FPS, bitrate, codec, replay length — from a
  hotkey or the Quick Panel, and unlike the old profiles it is yours to edit in a text file.

**What this means when you upgrade:** custom profiles saved in 1.0.0.0 are no longer loaded. The files
are not deleted — they stay in `%LocalAppData%\Luna Capture\Profiles\` if you want to read the values
out of them by hand before removing the folder. Your *current* settings are untouched; only the
ability to swap between stored snapshots is gone.

> "Replay source profile" on the Replay page is a **different, unrelated** setting — it chooses which
> monitors the rolling buffer arms. It is unchanged.

---

## New: a Video & encoder page

Recording settings were scattered across the Record page while Instant Replay had none of its own,
even though both produce the same kind of file. There is now an **Encoder** entry in the sidebar
holding everything that shapes a video, applying to recordings and Replay clips alike:

| Setting | What it does |
| --- | --- |
| Processing load | CPU / GPU / Balanced — see below |
| **Codec** | Twelve of them now — see below |
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

The Record and Replay pages no longer carry duplicate copies of these controls. What they show now is
a read-only summary that takes you to the Encoder page, so there is exactly one place to change any of
it and no way for two screens to disagree.

The new encoder controls go through `ICodecAPI`. Hardware encoders differ in what they implement, so a
property a driver rejects simply leaves that encoder's own default in place rather than failing the
recording. Settings are locked while a recording or Replay buffer is live, and the page says so.

## New: twelve codecs, not three

1.0.0.0 offered H.264, HEVC and AV1, always in MP4. The full list is now:

| Codec | Container | Notes |
| --- | --- | --- |
| H.264 / AVC | MP4 | Universal. Hardware encoders everywhere. |
| HEVC / H.265 | MP4 | Roughly 30 % smaller than H.264 at the same quality. |
| AV1 | MP4 | Best compression; needs Arc / RTX 40 / RX 7000-class hardware. |
| VP9 | MP4 | Royalty-free; needs a vendor encoder. |
| H.264 fragmented | fragmented MP4 | Survives a crash mid-recording. |
| HEVC fragmented | fragmented MP4 | The same, in H.265. |
| MPEG-4 Part 2 | MP4 | DivX/Xvid era; plays on very old devices. |
| H.263 | 3GP | Legacy mobile. |
| Windows Media Video 9 | WMV | Native Windows playback. |
| VC-1 Advanced | WMV | Windows Media's successor to WMV9. |
| Motion JPEG | AVI | Every frame a keyframe — huge files, trivially editable. |
| H.264 in AVI | AVI | For editors that refuse MP4. |

The container and the file extension follow the codec automatically — including for Instant Replay,
whose cached segments and final merged clip both use it. Choosing a codec your machine has no encoder
for fails when the recording starts, with a message naming the codec and suggesting H.264, rather than
producing a broken file. In the AVI and WMV containers the AAC audio track may be dropped; you get a
warning and the video still records.

## New: Processing load

Recording used to always take the same route through the machine, and on a CPU with a small cache that
costs real frames in game. **Processing load** has three modes, applied to normal recordings *and*
Instant Replay:

- **Balanced** — the previous behaviour: hardware encoder, GPU resize when downscaling.
- **Prefer GPU** — least CPU cost. Hardware encoder forced, resize kept on the GPU.
- **Prefer CPU** — software encoder and CPU resize, leaving the GPU entirely to the game. For machines
  that are GPU-limited rather than CPU-limited.

> **If a recording costs you frames, also try lowering Resolution below your monitor's native
> height.** That switches the resize onto the GPU *and* shrinks both the frame readback and the
> encode — on a weak CPU it is the single biggest one-click win, bigger than the mode above.

## New: move and resize a region after drawing it

The region editor gained a second tool, **Move** (<kbd>2</kbd>, or <kbd>M</kbd>), and drops into it
automatically the moment you finish dragging a selection — so reframing what you just drew takes no
extra clicks. Inside it:

- **Drag the region** to slide it, with everything you have drawn travelling along with it.
- **Drag one of the eight edge or corner handles** to resize.
- **Drag a drawing** — an arrow, a box, a pen stroke — to move just that one. It highlights as you
  pass over it, and hit-testing follows the ink rather than the bounding box, so the hollow middle of
  a large rectangle still belongs to the region underneath it.
- **Drag outside the region** to start a fresh selection, without switching tools.

The annotation tools shifted one number along as a result: <kbd>3</kbd> arrow, <kbd>4</kbd> rectangle,
<kbd>5</kbd> pen, <kbd>6</kbd> highlighter, <kbd>7</kbd> eraser. Their letter shortcuts are unchanged.

## New: a real control bar in fullscreen video

Fullscreen video playback had exactly one control — an *Exit fullscreen* button. It now has a proper
HUD along the bottom of the screen: a draggable seek bar, play/pause, ±10 s skip, elapsed and total
time, a volume slider with mute, and the exit button.

It **fades out after about 2.5 seconds of no mouse movement, taking the cursor with it**, and fades
back in the instant you move — so a fullscreen video is not permanently covered by its own chrome. It
stays put while the pointer is over it or you are dragging a slider. New keys while fullscreen:
<kbd>←</kbd>/<kbd>→</kbd> seek 5 s, <kbd>↑</kbd>/<kbd>↓</kbd> volume, <kbd>M</kbd> mute, alongside the
existing <kbd>Space</kbd>, <kbd>Esc</kbd>, <kbd>F</kbd> and double-click.

---

## Fixes

### Multi-monitor

**The Quick Panel drew a shadow onto the next monitor, and its slide-in animation crossed the screen
boundary.** Two separate causes. Its entrance animation started off the left edge of the target
monitor — which, on a secondary monitor, is physically the primary one. And it asked the compositor to
round its corners while also clipping itself to a rounded region, so Windows drew the panel's shadow
sized to the *unclipped* rectangle. The panel now animates entirely within the monitor it opens on, as
a short slide plus a fade, and no longer double-rounds.

**Region-capture instructions were split across the gap between two screens.** The region editor
covers the whole virtual desktop, and its hint text was centred on *that* — which on a multi-monitor
rig is the seam between monitors, readable on neither. The instructions and the window-picker prompt
are now anchored to your primary monitor. Selecting a region still works across every screen.

**The recording preview froze whenever you Alt-Tabbed away, even with Luna fully visible on a second
monitor.** The preview was paused whenever Luna was not the foreground window — which is right when a
game is covering it, and wrong when the game is on a different screen and Luna is in plain sight. It
now keeps running when the foreground window is on another monitor, and still pauses when something is
genuinely on top of it.

### Capture and interface

**Encoder settings were clipped mid-sentence.** Every card on the new Encoder page was a fixed height,
so any description that wrapped to a second line lost it — "Who does the resizing and encod…". Cards
now measure their own text and grow to fit, which also means they survive a translation plugin or a
theme that scales the font up.

**Region screenshots were blurry while Instant Replay was running.** With a recording height below
native, the recorder's CPU-side frame is downscaled for the encoder — and screenshots were being taken
from *that* frame and stretched back up. Both the region editor's frozen preview and the saved file
now come from a full-resolution snapshot. Since a replay buffer that resumes on launch is common, this
affected most region captures.

**Test notification made no sound.** The button in Settings showed the toast but never played the
notification sound, so there was no way to check the sound you had chosen. It now does both, and says
which.

**Explorer showed captures in a seemingly random order.** The file names were always sortable
(`YYYY-MM-DD_HH-MM-SS_mmm_…`), but the folders sit under Pictures/Videos and inherit those library
templates, which sort by *Date taken* — metadata screen captures never carry. The capture folders are
now pinned to Explorer's generic template, so they sort by name, which is date order.
*If you already opened those folders, Windows remembers the old view: sort by Name once.*

**Notification statuses were misleading.** Failures such as "Recorder is busy" or "Replay buffer is
off" were shown with a green success tick. Toasts now have three states: green check, amber warning
triangle, red error cross.

### Setup and plumbing

**Opening a `.luna` plugin did nothing.** It relied on handing the file to a running copy, which failed
silently when the window was hidden in the tray, and was blocked outright by Windows when the app ran
elevated. Opening a plugin now shows its own small installer — plugin name, author, description and
the exact permissions it requests — and installs it directly, working the same whether Luna is closed,
in the tray, or elevated.

**Update checking barely worked.** It ran at most once every 20 hours and only at launch, so a release
published after startup stayed invisible. It now checks on launch and every 6 hours, shows a persistent
banner when an update exists, and Settings has a **Check for updates** button that always answers —
including a "Check again" that works while an update banner is already up.

**Autostart and "always run as administrator" can now be enabled together.** A Startup shortcut cannot
do both — Windows silently skips it, because the sign-in launcher never shows a UAC prompt. Selecting
both now registers a logon task with the highest available privileges instead. Creating that task needs
an elevated installer; if it is not, the installer falls back to a plain shortcut and says so.

**The uninstaller opened the installer.** Setup and uninstall are the same executable — installing
copies it into the install folder as `LunaUninstall.exe` and registers it with a `--uninstall`
argument. Removing Luna through Windows' *Apps & features* therefore worked, but double-clicking
`LunaUninstall.exe` in the install folder — the obvious thing to do with a file called that — passed
no argument, so it came up as Setup and offered to install over itself. It now decides from its own
filename as well as the argument, and uninstalls either way.

**Message boxes replaced.** Every stock Windows dialog is gone; deleting a file now shows a themed
confirmation with the file name, size and date, and a red destructive button.

**The window opens above other windows from the tray** — and refuses to open at all over a game in true
exclusive fullscreen, telling you to Alt-Tab first, rather than opening invisibly behind it and
stealing focus. The title bar now has only a close button; minimising did the same thing as closing,
since the window has no taskbar button.

---

## For plugin authors

Two **breaking** changes to LunaScript, both a direct consequence of the feature changes above:

- **`ui page profiles` no longer parses** — that page does not exist. `ui page encoder` was added in
  its place, and reaches the new Video & encoder page.
- **`setting codec` takes twelve identifiers**, not three: `h264`, `hevc`, `av1`, `vp9`,
  `h264_fragmented`, `hevc_fragmented`, `mpeg4`, `h263`, `wmv9`, `vc1`, `mjpeg`, `h264_avi`. Existing
  scripts using `h264`, `hevc` or `av1` are unaffected.

`plugins/` ships five ready-made plugins, including a **translation base** containing every
user-facing string in the app (710 entries) as a starting point for any language; it has been updated
for the strings this release adds, changes and removes. All five are parsed by the test suite on every
build, so they cannot silently rot. The offline SDK reference in `docs/plugin-sdk.html` is updated to
match.

## For contributors

- The version lives in exactly one place — the `VERSION` file at the repository root — and propagates
  to the C++ build, both executables' file properties, the build scripts and the website.
- `prepare_github_upload.bat` stages a clean, minimal source folder for uploading through the GitHub
  website, which accepts at most 100 files per drag.
- Codec, container and file extension are one table (`CodecProfile` in `RecordingEngine.cpp`). They
  have to travel together — asking the MP4 muxer for Motion JPEG, or the AVI muxer for HEVC, fails even
  when the encoder itself exists. Anything deriving a filename or a muxer goes through it.
- The first three `VideoCodec` enum values are pinned: `settings.ini` stores the raw ordinal, so
  renumbering them would silently change an existing user's codec.
- A fix worth knowing about: `resources.rc` never defined `VS_VERSION_INFO`, so every previous build
  shipped with **no readable version in its file properties**. Both executables now carry it.

---

## Known limitations

Mostly unchanged from 1.0.0.0, and worth reading before filing a bug:

**Games in *true* exclusive fullscreen are a platform limitation.** Windows stops compositing that
monitor's desktop image, so Desktop Duplication has nothing to hand over — recording can freeze on the
last frame from before the transition, and overlays cannot be drawn on top at all. **Workaround:** set
the game to *Borderless* / *Windowed Fullscreen*. Modern flip-model fullscreen is unaffected.

**Replay previews can appear black on a multi-GPU setup**, when a monitor is driven by a different
adapter than the capture device. Recording itself is unaffected.

**Codecs past H.264 depend on your hardware.** The list is offered in full rather than probed and
filtered, because encoder support varies by GPU, driver and Windows version. If a codec has no encoder
on your machine, starting a recording says so immediately and names what to pick instead.

**A window covering Luna on the same monitor still pauses the preview**, even if it only covers part of
it. Windows exposes no reliable "am I occluded" signal, so the check is per-monitor.

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
