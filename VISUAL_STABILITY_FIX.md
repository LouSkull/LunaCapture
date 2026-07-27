# Visual stability fix — application version 1.0.0.0

## Included changes

- compact 238 px custom dark tray popup;
- consistent native vector icons across the main window, tray, overlay, and region editor;
- persistent off-screen region-editor compositor to remove flashing while selecting and drawing;
- visible region action bar with Arrow, Rectangle, Pen, Highlighter, Undo, Copy, Save, and Cancel;
- toolbar hover and press states;
- clipboard compatibility path using CF_DIBV5, bottom-up CF_DIB, and CF_BITMAP;
- clipboard contention retry and opaque-alpha normalization;
- additional null-handle checks in region rendering.

The visible application version remains exactly `1.0.0.0`.
