#pragma once

#include "Types.hpp"

#include <Windows.h>

#include <cstdint>
#include <memory>
#include <vector>

class RegionEditor {
public:
    RegionEditor() = default;
    ~RegionEditor();

    bool Create(HINSTANCE instance, HWND notifyWindow);
    void Destroy();
    enum class SessionMode { Region, WindowPicker };

    bool Show(std::shared_ptr<FrameBuffer> frame, SessionMode mode, const AppSettings& settings, const std::wstring& filenamePrefix);
    void Cancel();
    void RefreshTheme();
    bool IsOpen() const noexcept { return visible_; }
    HWND WindowHandle() const noexcept { return hwnd_; }

private:
    // Select draws a brand-new region; Move repositions and resizes the one that
    // already exists (Lightshot-style), leaving the annotations anchored to the
    // frozen desktop underneath.
    enum class Tool { Select, Move, Arrow, Rectangle, Pen, Highlighter, Eraser };
    // Which part of the selection a Move drag grabbed. Grabbing an annotation
    // instead is tracked separately by movingAnnotation_.
    enum class Grip { None, Body, Left, Right, Top, Bottom, TopLeft, TopRight, BottomLeft, BottomRight };
    struct Annotation {
        Tool tool = Tool::Arrow;
        COLORREF color = RGB(105, 175, 255);
        BYTE alpha = 255;
        POINT start{};
        POINT end{};
        std::vector<POINT> points;
    };
    enum class ToolbarAction { None, Select, Move, Arrow, Rectangle, Pen, Highlighter, Eraser, Undo, Redo, Color, Copy, Save };
    enum class PaletteTarget { None, SatVal, Hue, Opacity, Swatch };
    struct ToolbarItem { ToolbarAction action; int width; };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    void Paint();
    bool RecreateFonts();
    bool BuildFrameSurface();
    void ReleaseFrameSurface();
    void DrawToolbar(HDC dc, const RECT& selection);
    std::vector<ToolbarItem> ToolbarItems() const;
    static bool IsGroupStart(ToolbarAction action);
    ToolbarAction HitTestToolbar(POINT point) const;
    RECT ToolbarItemRect(ToolbarAction action) const;
    void ExecuteToolbar(ToolbarAction action);
    void Undo();
    void Redo();
    void Complete(RegionResultAction action);
    FrameBuffer RenderCrop() const;
    void DrawAnnotations(HDC dc, POINT offset) const;
    void DrawAnnotation(HDC dc, const Annotation& annotation, POINT offset) const;
    RECT NormalizedSelection() const;
    bool HasSelection() const;
    void UpdateToolbarRect();
    void SetTool(Tool tool);
    void UpdateHoveredWindow(POINT clientPoint);
    // Instruction text and the window-picker prompt are confined to this rect. The
    // editor spans the whole virtual desktop, so centring on the client area put
    // the text on the seam between two monitors - readable on neither.
    RECT PrimaryMonitorClientRect() const;
    Grip GripAt(POINT point) const;
    void ApplyGripDrag(POINT point);
    void DrawSelectionGrips(HDC dc, const RECT& selection) const;
    // Index of the topmost annotation whose ink is under `point`, or -1. Hit
    // testing follows the stroke, not the bounding box, so the hollow middle of
    // a rectangle still belongs to the selection underneath it.
    int AnnotationAt(POINT point) const;
    void ApplyAnnotationDrag(POINT point);
    static void OffsetAnnotation(Annotation& annotation, int dx, int dy);
    RECT WindowChoicePanel() const;
    RECT WindowChoiceButton(int index) const;
    void EraseAt(POINT point);

    // Color palette popup ("choose color through a nice palette").
    void OpenPalette();
    void ClosePalette();
    void LayoutPalette();
    void DrawPalette(HDC dc);
    PaletteTarget PaletteControlAt(POINT point, int* swatchIndex) const;
    void ApplyPalettePoint(PaletteTarget target, POINT point);
    void SyncHsvFromColor();
    void SyncColorFromHsv();
    void SetColor(COLORREF color);

    HINSTANCE instance_ = nullptr;
    HWND notifyWindow_ = nullptr;
    HWND hwnd_ = nullptr;
    std::shared_ptr<FrameBuffer> frame_;
    AppSettings settings_{};
    std::wstring pendingSavePath_;
    std::wstring filenamePrefix_;
    ImageFormat pendingSaveFormat_ = ImageFormat::Png;
    bool visible_ = false;
    bool draggingSelection_ = false;
    bool drawingAnnotation_ = false;
    POINT dragStart_{};
    POINT dragCurrent_{};
    RECT selection_{};
    Grip activeGrip_ = Grip::None;
    Grip hoveredGrip_ = Grip::None;
    POINT gripDragStart_{};
    RECT gripStartSelection_{};
    // Snapshot taken when a Move drag starts, so every frame applies one absolute
    // delta to the original geometry. Accumulating per-frame deltas would drift
    // once the selection clamps against the desktop edge.
    std::vector<Annotation> gripStartAnnotations_;
    int hoveredAnnotation_ = -1;
    int movingAnnotation_ = -1;
    POINT annotationDragStart_{};
    Annotation annotationDragOrigin_{};
    RECT toolbarRect_{};
    SessionMode mode_ = SessionMode::Region;
    Tool tool_ = Tool::Select;
    Annotation currentAnnotation_{};
    std::vector<Annotation> annotations_;
    std::vector<Annotation> redoAnnotations_;
    ToolbarAction hoveredToolbar_ = ToolbarAction::None;
    HWND hoveredWindow_ = nullptr;
    RECT hoveredWindowRect_{};
    bool windowChoicePending_ = false;
    ToolbarAction pressedToolbar_ = ToolbarAction::None;
    COLORREF currentColor_ = RGB(96, 176, 255);
    BYTE highlighterAlpha_ = 96;

    bool paletteOpen_ = false;
    PaletteTarget paletteDrag_ = PaletteTarget::None;
    float hsvH_ = 210.0f;
    float hsvS_ = 0.62f;
    float hsvV_ = 1.0f;
    RECT paletteRect_{};
    RECT paletteSatValRect_{};
    RECT paletteHueRect_{};
    RECT paletteOpacityRect_{};
    RECT paletteSwatchRect_{};
    HDC frameDc_ = nullptr;
    HBITMAP frameBitmap_ = nullptr;
    HGDIOBJ frameOldBitmap_ = nullptr;
    HDC dimmedDc_ = nullptr;
    HBITMAP dimmedBitmap_ = nullptr;
    HGDIOBJ dimmedOldBitmap_ = nullptr;
    HDC renderDc_ = nullptr;
    HBITMAP renderBitmap_ = nullptr;
    HGDIOBJ renderOldBitmap_ = nullptr;
    HFONT toolbarFont_ = nullptr;
    HFONT hintFont_ = nullptr;
    HFONT sizeFont_ = nullptr;
};
