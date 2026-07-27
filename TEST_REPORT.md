# LunaShot 1.0.0.0 test report

## Automated checks included in the project

- C++20 core filename, path, ellipsis, and QPC timing tests.
- Windows SDK header smoke target for Shell, D3D11, DXGI Desktop Duplication, and WIC types.
- CMake Release build with `/W4`, `/permissive-`, Control Flow Guard, ASLR, DEP, and static MSVC runtime.
- CTest execution before the executable is copied to `dist`.

## Source checks performed for this package

- all application-facing strings were checked for Cyrillic text;
- all resource, manifest, UI, tray, and executable version fields are `1.0.0.0`;
- no Electron, Node.js, Python, WebView, or browser source/runtime remains;
- region editor actions and tray commands are routed through dedicated Win32 messages;
- screenshot encoding remains outside the capture thread;
- the main UI, tray menu, region editor fonts, and notification overlay use cached GDI resources;
- the installer compares source timestamps with the existing executable, so later source changes rebuild while the visible version remains `1.0.0.0`;
- all-display transparent gaps are normalized to opaque black.

## Environment limitation

The complete Win32 executable cannot be linked in the Linux packaging environment because MSVC and the Windows SDK are unavailable here. `build_release.bat` performs the authoritative MSVC build and tests on Windows.

## Package validation result

- portable core tests: **passed**;
- source/header brace and local-include checks: **passed**;
- English-only application string scan: **passed**;
- exact `1.0.0.0` resource/manifest/CMake version checks: **passed**;
- duplicate-manifest static check: **passed**;
- default three-shortcut and region-tool feature checks: **passed**;
- cached UI/tray/overlay resource checks: **passed**;
- WAV and ICO asset integrity checks: **passed**.

## Compact keyboard hotkey recorder fix

- shortcut learning now uses `WH_KEYBOARD_LL`;
- Raw Input keyboard events provide a second detection path;
- media/app-command keys are mapped to their Windows virtual keys;
- `Fn` is intentionally represented by the final Windows key produced by keyboard firmware;
- the existing focused-window `WM_KEYDOWN` path remains as a fallback;
- duplicate tray command dispatch was removed.

## Compact tray, icon, anti-flicker, and clipboard pass

- custom vector icon coverage checked for the main UI, tray commands, notification overlay, and every region-editor action;
- tray width reduced to 238 px and row height reduced to 30 px;
- tray popup uses a rounded custom window region and never invokes the white system menu;
- region editor now owns a persistent render surface in addition to frozen and dimmed surfaces;
- region paint path presents one completed off-screen frame per `WM_PAINT`;
- every custom window returns handled from `WM_ERASEBKGND`;
- copy path exports `CF_DIBV5`, bottom-up `CF_DIB`, and `CF_BITMAP`;
- clipboard image rows are vertically normalized and alpha is forced to 255;
- clipboard contention retry path is present;
- portable core tests: **passed** after the changes;
- C++ brace, local include, icon-reference, and CMake source-list checks: **passed**.

## Undo / Redo history validation

- Toolbar count and width arrays both contain 9 items.
- Undo moves the last annotation into the redo stack.
- Redo moves the last undone annotation back into the visible annotation stack.
- Ctrl+Z, Ctrl+Y and Ctrl+Shift+Z are wired in the region editor message loop.
- A new selection and a new annotation clear the redo branch.
- Undo and Redo icons are mirrored classic curved arrows.
- Disabled history buttons are visually dimmed and execute as safe no-ops.
- Visible application version remains 1.0.0.0.


## Region Save As static checks

- Native `IFileSaveDialog` path is wired only to Region Editor Save.
- Cancel leaves the Region Editor open.
- Selected extension controls PNG/JPEG encoding.
- Explicit output path reaches the background save queue.
- Active-window click capture remains instant and does not open Save As.
