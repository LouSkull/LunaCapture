# Luna Capture 1.0.0.0

A lightweight native Windows screen-capture utility written in C++20. There is no Electron, Node.js, Python, WebView, or browser runtime.

> Formerly **LunaShot**. The app was renamed to **Luna Capture** as it grows from a pure screenshot tool toward screen **recording** and capture history / rollback. The executable is now `LunaCapture.exe`.

## Interface

The entire UI is drawn natively with anti-aliased GDI+ — every icon, card, toggle, and rounded panel is vector-rendered, and text uses grayscale anti-aliasing so it stays crisp with no colored fringing (on screen, over RDP, and inside screenshots). The main window, dark tray menu, notification overlay, and region editor all share one consistent icon set.

## Default shortcuts

- **Print Screen** — capture the display containing the foreground window. If Windows cannot resolve that window, Luna Capture uses the display under the cursor.
- **Ctrl + Print Screen** — capture every attached display and stitch them into one virtual-desktop image.
- **Shift + Print Screen** — open the full-desktop region editor.
- **Alt + Print Screen** — pick a single window to capture.

All four shortcuts can be changed inside the application.

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
- multi-display capture without browser processes;
- background WIC encoding and file writing;
- compact notification overlay with no shadow, blur, browser compositor, or delayed HTML rendering;
- UI, dialogs, notifications, tray menu, and error messages are English-only;
- the visible and file version is always `1.0.0.0`.

## Notes

DXGI Desktop Duplication is designed for the composed Windows desktop. Borderless fullscreen is the most reliable game mode. A true exclusive fullscreen application can bypass normal desktop overlays; Luna Capture does not inject code into games.

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
