#pragma once

#include <Windows.h>

enum class UiIcon {
    Home,
    Capture,
    Settings,
    Monitor,
    Window,
    AllDisplays,
    Region,
    Folder,
    Sound,
    Bell,
    Power,
    Arrow,
    Rectangle,
    Pen,
    Highlighter,
    Eraser,
    Undo,
    Redo,
    Copy,
    Save,
    Close,
    Minimize,
    Check,
    Warning,
    Photo,
    Move,
};

void DrawUiIcon(HDC dc, UiIcon icon, const RECT& bounds, COLORREF color, int stroke = 2);
