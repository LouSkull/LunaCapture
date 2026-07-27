# Window Capture + UI Alignment Fix

- Added a fourth shortcut: **Active window** (default `Alt + Print Screen`).
- Added a dedicated window icon and tray action.
- Settings rows now have fixed icon, text, and hotkey columns.
- Reset card text no longer overlaps the icon.
- Tray labels and shortcuts are rendered in independent columns with ellipsis fallback.
- Active-window capture uses DWM extended frame bounds and crops the already-cached DXGI desktop frame.
- Visible product version remains **1.0.0.0**.
