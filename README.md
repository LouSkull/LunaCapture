# Luna Capture 1.0.0.0

A lightweight native Windows screenshot, screen-recording, and instant-replay utility written in C++20. There is no Electron, Node.js, Python, WebView, or browser runtime.

> Formerly **LunaShot**. The executable is now `LunaCapture.exe`.

## Screen recording studio

The **Record** page contains the full recording workflow:

- H.264, HEVC (H.265), or AV1 MP4 recording with Start/Stop controls and an `F9` global shortcut;
- DXGI Desktop Duplication capture with practical FPS presets from 24 to 240 plus an optional custom value;
- automatic foreground-game monitor selection (or a fixed monitor override);
- Windows 10 `DuplicateOutput1` capture with an automatic legacy DXGI fallback;
- BGRA, RGBA, 10-bit HDR, and 16-bit-float desktop-frame conversion for fullscreen games;
- sustained-black-frame detection that switches the duplication backend instead of writing a black MP4;
- uninterrupted recovery after exclusive-fullscreen transitions, Alt+Tab, display-mode changes, and GPU-device resets;
- a stable recording canvas that letterboxes a new game resolution instead of terminating or corrupting the active MP4;
- active-display screenshots taken directly from the live recorder frame while recording/replay continues;
- MMCSS `Capture` scheduling and missed-frame timeline correction to avoid a CPU catch-up spike after reconnection;
- hardware Media Foundation encoders when the installed GPU/driver exposes them, with a Windows fallback where available;
- selectable display, native/720p/1080p/1440p/4K/8K output presets, bitrate presets, and optional custom values;
- DXGI hardware-pointer-shape cursor compositing with a regular Windows-cursor fallback;
- optional WASAPI loopback capture of the current Windows system mix;
- an explicitly switchable live letterboxed preview that reuses the recorder frame and never opens a second capture session while recording;
- a large 640×360 preview surface with status overlaid on the image instead of consuming a separate side panel;
- live elapsed-time, encoded-FPS, frame, and dropped-frame metrics;
- a separate recording folder under the user's Videos library by default.

## Audio mixer

The dedicated **Audio** page can record the Windows system mix or isolate one running application, plus as many as four microphone endpoints at the same time. System, application, and every microphone input have independent 0–200% gain controls. Sources are converted and mixed to 48 kHz stereo with a soft limiter, then encoded as AAC. Device IDs and gains are persisted, so USB microphones reconnect to the same mixer slot. Adding or changing a microphone opens an exact named-device selector, and every active microphone row has a live input meter plus a Windows-mute warning.

FPS, video bitrate, replay length, preview rate, output size, notification duration, and source gains open fast dark preset lists; **Custom** remains the last option when an exact value is useful. AAC shows only encoder-supported choices. Display, microphone, and application sources use direct named selectors.

Per-application process loopback is exposed by Windows starting with build 20348. Standard Windows 10 builds can use system audio and multiple microphones, but application-only audio gracefully reports that the OS API is unavailable instead of stopping video recording.

## Instant replay / rollback

Replay Buffer continuously writes short encoded MP4 segments into `%LocalAppData%\Luna Capture\ReplayCache` instead of retaining raw BGRA frames in RAM: continuous capture is required to save something that already happened, but the cache is a bounded rolling window rather than an ever-growing recording. Segment size is chosen from the requested replay duration, and finalized segments older than the selected window are deleted immediately. The UI reports `buffered / selected rolling` instead of showing session uptime. One or several monitors can be buffered simultaneously; `F10` saves all selected monitors, while optional independent shortcuts save only a chosen monitor. Every channel uses a separate cache and output name.

While recording or Replay Buffer owns DXGI, screenshots no longer require stopping it. Active-display captures use the recorder's full-resolution frame; all-display, region-editor, and window-picker captures combine full-resolution frames from every active replay channel and use an on-demand desktop fallback for monitors that are not buffered. The normal screenshot save worker remains alive during this handoff.

The sidebar has a dedicated **Replay** page; multi-monitor controls are no longer hidden behind an `R-HUB` popup chain. The page shows large Start/Stop and Save All actions, visible **Auto game / Primary only / All monitors** profiles, rolling duration, Save All binding, output folder, and one live card per connected display. Each card has its source toggle, `LIVE / ARMED / AUTO / OFF` state, direct shortcut control, and **Save this** action. The Record page's Replay chip and the tray's Replay Control Center item navigate directly to this page. While replay is live, the tray also exposes one direct Save row for every channel that is actually buffering.

Encoder-pipeline settings (codec, FPS, bitrate, display sources, cursor and audio mixer) are visibly locked for the lifetime of an active recording/replay session because changing them would require rebuilding the MP4 writer. Screenshot settings, preview, notifications, navigation, and every global shortcut remain editable. The Settings page exposes all six primary bindings; the replay row also opens per-monitor bindings when multi-monitor replay is configured.

## Custom notification sounds

The Capture page can import PCM or IEEE-float `.wav` files. Imported files are copied into `%LocalAppData%\Luna Capture\Sounds` with collision-safe names, so moving or deleting the original does not break Luna Capture. The selected sound is preloaded through `waveOut` and is used for capture, recording, and replay feedback. Every previously imported sound remains in the managed sound library; **Default** returns to the embedded Luna sound.

After selecting a window with **Alt + Print Screen**, Luna Capture keeps the frozen window highlighted and presents three explicit actions: save to the default folder, copy to the clipboard, or save and copy.

## Interface

The entire UI is drawn natively with anti-aliased GDI+ — every icon, card, toggle, and rounded panel is vector-rendered, and text uses ClearType Natural rendering. The main window, recording studio, dark tray menu, notification overlay, region editor, preset lists, and numeric popups share the same dark rounded chrome. Owner-drawn buttons clear their full child surface before painting, avoiding bright square corner artifacts on Windows 10.

## Default shortcuts

- **Print Screen** — capture the display containing the foreground window. If Windows cannot resolve that window, Luna Capture uses the display under the cursor.
- **Ctrl + Print Screen** — capture every attached display and stitch them into one virtual-desktop image.
- **Shift + Print Screen** — open the full-desktop region editor.
- **Alt + Print Screen** — pick a single window to capture.

All four shortcuts can be changed inside the application.

- **F9** — start/stop screen recording.
- **F10** — save the current instant replay.

The recording shortcuts are edited directly on the Record page. Every screenshot, recording, and replay binding now uses three deduplicated input routes: background Raw Input for exclusive-fullscreen games, a low-level keyboard fallback, and normal Windows `RegisterHotKey`. A game owning the ordinary hotkey no longer disables `F9`, `F10`, or the screenshot bindings. The accepted route is written to `%LocalAppData%\Luna Capture\capture.log` for fullscreen diagnostics.

## Region editor

1. Drag to select an area.
2. Use the compact toolbar or keyboard shortcuts:
   - `1` / `V` select · `2` / `A` arrow · `3` / `R` rectangle · `4` / `P` pen · `5` / `H` highlighter · `6` / `E` eraser
   - `Ctrl + Z` undo · `Ctrl + Y` / `Ctrl + Shift + Z` redo
   - `C` copy · `S` or `Enter` save as · `Esc` cancel
3. The editor uses the frozen frame that was captured when the shortcut was pressed.

### Color palette popup

The toolbar shows a single color swatch. Click it to open a palette popup with a saturation/value square, a hue spectrum bar, and a row of quick-pick swatches — drag anywhere to choose any color. Quick keys `B` `G` `T` `Y` `W` still set blue / green / red / yellow / white.

### Highlighter opacity

The highlighter has its own opacity slider (shown in the palette popup while the highlighter is active, adjustable with `[` / `]`). Opacity now changes **only the marker stroke** — the rest of the selection keeps its full brightness.

## Build and run

Run:

```bat
install_and_run.bat
```

The script checks for CMake and the MSVC x64 C++ toolchain, builds a static-runtime Release executable, runs the core tests, and starts:

```text
dist\LunaCapture.exe
```

## Design goals

- fixed-size, low-overhead native interface with a persistent back buffer and cached fonts;
- fully anti-aliased GDI+ rendering for icons, panels, toggles, and annotations;
- lightweight hover, press, toggle, and navigation animations that do not use Windows system animations;
- custom dark tray menu instead of the white Windows popup menu;
- cached tray and notification graphics so hover and capture feedback do not allocate a new surface every frame;
- cached DXGI frames for fast active-display captures;
- Media Foundation H.264/HEVC/AV1 MP4 recording with hardware-transform preference;
- WASAPI shared-mode loopback system-audio capture;
- multi-endpoint WASAPI microphone mixing and process-loopback audio where the OS supports it;
- preview downsampling directly from the cached GPU/readback frame, with queues and recycled surfaces released immediately when preview is off;
- encoded rotating replay segments instead of an unbounded raw-frame RAM queue;
- simultaneous multi-display replay with per-monitor cache workers and shortcuts, without browser processes;
- background WIC encoding and file writing;
- compact notification overlay with no shadow, blur, browser compositor, or delayed HTML rendering;
- UI, dialogs, notifications, tray menu, and error messages are English-only;
- the visible and file version is always `1.0.0.0`.

## Notes

DXGI Desktop Duplication supports normal desktop windows, borderless games, and DirectX/OpenGL exclusive-fullscreen modes. Windows invalidates the duplication interface when a game changes the display producer, HDR format, or resolution; Luna Capture listens for that transition, holds the last good frame, alternates modern and legacy duplication backends, recreates the GPU path, and continues the same recording timeline. Screenshot capture releases its duplication channel before recording/replay claims it and automatically restores it when recording ends. Backend selection and reconnect errors are written to `%LocalAppData%\Luna Capture\capture.log`.

When the foreground window covers the selected monitor, Luna rebinds its non-activating overlay to the topmost band throughout the animation and also sends a native Windows tray notification as a fullscreen fallback. The overlay never takes keyboard focus and is excluded from subsequent screenshots. A true legacy exclusive surface may still let Windows decide which system overlays are composited; Luna does not inject DLLs, hook the game renderer, modify the game process, or attempt to bypass anti-cheat/content protection. Secure-desktop and DRM/protected surfaces remain unavailable to normal applications.

## Compact keyboards and Fn shortcuts

Shortcut recording uses three Windows input paths: a low-level keyboard hook, Raw Input, and app-command media events. Press the physical shortcut exactly as you normally use it, including `Fn` when required.

Most keyboards process `Fn` inside their firmware, so Windows does not receive a separate `Fn` key. Luna Capture therefore records the final Windows key produced by the combination. For example, `Fn + P` may be displayed and stored as `Print Screen`. This is expected and the shortcut will still work.

A combination that only changes keyboard hardware state, such as backlight brightness or an internal profile, cannot be assigned because Windows receives no key event at all.

## Visual stability and clipboard compatibility

The region editor composes every frame into a persistent off-screen surface and presents it with one blit. Background erase is disabled, so selection, pen, rectangle, arrow, toolbar hover, and undo no longer expose intermediate frames or flash the desktop.

Clipboard export publishes the selected image in three Windows formats for compatibility:

- `CF_DIBV5` with explicit BGRA masks;
- bottom-up `CF_DIB` for older applications;
- `CF_BITMAP` as a fallback.

Clipboard opening is retried briefly when another application temporarily owns it. Pixel alpha is normalized to opaque before publishing.

## Region Save As

In the Region Editor, **Save as** opens the native Windows save dialog. Choose the folder, filename, and PNG/JPEG format. Cancel returns to the editor without losing the current selection or annotations.

## Persistent region selection

In the Region Editor, **Select** is the first tool and stays active until another tool is chosen. Drag anywhere — including inside the current selection — to replace it with a new region. Press `1` or `V` to return to Select.
