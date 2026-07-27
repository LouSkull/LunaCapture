#include "MediaViewerWindow.hpp"

#include "Types.hpp"
#include "UiGfx.hpp"
#include "UiIcons.hpp"

#include <mfplay.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>

namespace {
constexpr wchar_t kVideoHostClass[] = L"LunaCaptureVideoHost";
constexpr wchar_t kFullscreenHudClass[] = L"LunaCaptureVideoFullscreenHud";
constexpr UINT kPlayerEventMessage = WM_APP + 91;
constexpr LONGLONG kOneSecond100ns = 10000000LL;

// Fullscreen HUD geometry, in HUD-client pixels.
constexpr int kHudHeight = 108;
constexpr int kHudBottomMargin = 30;
constexpr int kHudSideMargin = 40;
constexpr int kHudMaxWidth = 1240;
constexpr int kHudRadius = 16;
constexpr int kHudPad = 22;
constexpr int kSeekCenterY = 30;
constexpr int kSeekThickness = 6;
constexpr int kControlsCenterY = 74;
constexpr int kButtonSize = 42;

// Idle handling. The HUD fades out once the pointer has been still for this long
// and fades back in the moment it moves again, so a fullscreen video is not
// permanently covered by its own chrome.
constexpr UINT kHudTimer = 1;
constexpr UINT kHudFrameMs = 33;
constexpr ULONGLONG kHudIdleTimeoutMs = 2600;
constexpr float kHudFadeStep = 0.14f;
constexpr int kPointerMoveThreshold = 3;
}  // namespace

struct MediaViewerWindow::Impl final : IMFPMediaPlayerCallback {
    std::atomic<ULONG> references{1};
    HINSTANCE instance = nullptr;
    HWND hwnd = nullptr;
    HWND parentEventWindow = nullptr;
    IMFPMediaPlayer* player = nullptr;
    std::wstring status;
    bool playing = false;
    bool muted = false;
    float volume = 1.0f;

    // True-fullscreen state. The video host is normally a WS_CHILD of the app
    // window; entering fullscreen re-parents it to the desktop as a popup so it
    // can cover the whole monitor instead of just the fixed-size app window.
    bool fullscreen = false;
    LONG_PTR savedStyle = 0;
    LONG_PTR savedExStyle = 0;

    // Fullscreen HUD: a separate layered popup carrying the transport controls.
    // It cannot be painted into the host window because Media Foundation owns
    // that surface and repaints over anything drawn on it.
    enum class HudItem { None, PlayPause, SkipBack, SkipForward, Seek, Mute, Volume, Exit };
    HWND hud = nullptr;
    RECT hudRect{};
    HudItem hudHot = HudItem::None;
    HudItem hudDragging = HudItem::None;
    float hudOpacity = 1.0f;
    bool hudWanted = true;
    ULONGLONG lastPointerActivity = 0;
    POINT lastPointerPos{};
    bool cursorHidden = false;
    HFONT hudFont = nullptr;
    HFONT hudTimeFont = nullptr;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMFPMediaPlayerCallback)) {
            *object = static_cast<IMFPMediaPlayerCallback*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references;
        if (remaining == 0) delete this;
        return remaining;
    }

    void STDMETHODCALLTYPE OnMediaPlayerEvent(MFP_EVENT_HEADER* event) override {
        if (!event || !hwnd) return;
        PostMessageW(hwnd, kPlayerEventMessage, static_cast<WPARAM>(event->eEventType),
                     static_cast<LPARAM>(event->hrEvent));
    }

    void DestroyPlayer() {
        if (player) {
            player->Shutdown();
            player->Release();
            player = nullptr;
        }
        playing = false;
    }

    void TogglePlaybackInternal() {
        if (!player) return;
        MFP_MEDIAPLAYER_STATE state = MFP_MEDIAPLAYER_STATE_EMPTY;
        if (FAILED(player->GetState(&state))) return;
        if (state == MFP_MEDIAPLAYER_STATE_PLAYING) player->Pause();
        else player->Play();
    }

    void ApplyVolume() {
        if (!player) return;
        player->SetVolume((std::max)(0.0f, (std::min)(1.0f, volume)));
        player->SetMute(muted || volume <= 0.0001f ? TRUE : FALSE);
    }

    LONGLONG DurationValue() const {
        if (!player) return 0;
        PROPVARIANT duration{};
        LONGLONG value = 0;
        if (SUCCEEDED(player->GetDuration(MFP_POSITIONTYPE_100NS, &duration))) {
            if (duration.vt == VT_I8) value = duration.hVal.QuadPart;
            else if (duration.vt == VT_UI8) value = static_cast<LONGLONG>(duration.uhVal.QuadPart);
        }
        PropVariantClear(&duration);
        return value;
    }

    LONGLONG PositionValue() const {
        if (!player) return 0;
        PROPVARIANT position{};
        LONGLONG value = 0;
        if (SUCCEEDED(player->GetPosition(MFP_POSITIONTYPE_100NS, &position))) {
            if (position.vt == VT_I8) value = position.hVal.QuadPart;
            else if (position.vt == VT_UI8) value = static_cast<LONGLONG>(position.uhVal.QuadPart);
        }
        PropVariantClear(&position);
        return value;
    }

    void SetPosition(LONGLONG value100ns) {
        if (!player) return;
        const LONGLONG duration = DurationValue();
        LONGLONG clamped = (std::max<LONGLONG>)(0, value100ns);
        if (duration > 0) clamped = (std::min)(clamped, duration - 1);
        PROPVARIANT position{};
        position.vt = VT_I8;
        position.hVal.QuadPart = clamped;
        player->SetPosition(MFP_POSITIONTYPE_100NS, &position);
    }

    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
            case WM_ERASEBKGND:
                return 1;
            case WM_SIZE:
                if (player) player->UpdateVideo();
                return 0;
            // In fullscreen the video surface owns the keyboard, so the app window's
            // own Esc/F handling never sees these presses — handle them here too.
            case WM_KEYDOWN:
                if (!fullscreen) break;
                // Any key counts as activity: a viewer driving playback from the
                // keyboard should see the HUD respond, not stay faded out.
                NoteHudActivity();
                if (wParam == VK_ESCAPE || wParam == 'F') {
                    RequestExitFullscreen();
                    return 0;
                }
                if (wParam == VK_SPACE) {
                    TogglePlaybackInternal();
                    return 0;
                }
                if (wParam == VK_LEFT) { SetPosition(PositionValue() - 5 * kOneSecond100ns); return 0; }
                if (wParam == VK_RIGHT) { SetPosition(PositionValue() + 5 * kOneSecond100ns); return 0; }
                if (wParam == VK_UP || wParam == VK_DOWN) {
                    volume = (std::clamp)(volume + (wParam == VK_UP ? 0.05f : -0.05f), 0.0f, 1.0f);
                    if (volume > 0.0001f) muted = false;
                    ApplyVolume();
                    if (parentEventWindow) PostMessageW(parentEventWindow, WM_APP_MEDIA_STATE_CHANGED, 0, 0);
                    return 0;
                }
                if (wParam == 'M') {
                    muted = !muted;
                    if (!muted && volume <= 0.0001f) volume = 0.5f;
                    ApplyVolume();
                    if (parentEventWindow) PostMessageW(parentEventWindow, WM_APP_MEDIA_STATE_CHANGED, 0, 0);
                    return 0;
                }
                break;
            case WM_LBUTTONDBLCLK:
                if (fullscreen) {
                    RequestExitFullscreen();
                    return 0;
                }
                break;
            case kPlayerEventMessage:
                if (wParam == MFP_EVENT_TYPE_PLAY) playing = true;
                else if (wParam == MFP_EVENT_TYPE_PAUSE || wParam == MFP_EVENT_TYPE_STOP ||
                         wParam == MFP_EVENT_TYPE_PLAYBACK_ENDED) playing = false;
                if (wParam == MFP_EVENT_TYPE_MEDIAITEM_SET) {
                    ApplyVolume();
                    if (player) player->UpdateVideo();
                }
                if (FAILED(static_cast<HRESULT>(lParam))) status = L"Playback error";
                if (parentEventWindow) PostMessageW(parentEventWindow, WM_APP_MEDIA_STATE_CHANGED, 0, 0);
                return 0;
            default:
                break;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        Impl* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<Impl*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd = window;
        }
        return self ? self->Handle(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
    }

    void RequestExitFullscreen() {
        if (parentEventWindow) PostMessageW(parentEventWindow, WM_APP_MEDIA_EXIT_FULLSCREEN, 0, 0);
    }

    int HudWidth() const { return (std::max<int>)(1, hudRect.right - hudRect.left); }

    RECT HudButtonRect(int centerX, int centerY, int size) const {
        const int half = size / 2;
        return RECT{centerX - half, centerY - half, centerX + half, centerY + half};
    }

    RECT SeekTrackRect() const {
        return RECT{kHudPad, kSeekCenterY - kSeekThickness / 2, HudWidth() - kHudPad, kSeekCenterY + kSeekThickness / 2};
    }

    RECT PlayRect() const { return HudButtonRect(kHudPad + kButtonSize / 2, kControlsCenterY, kButtonSize); }
    RECT SkipBackRect() const { return HudButtonRect(kHudPad + kButtonSize + 12 + kButtonSize / 2, kControlsCenterY, kButtonSize); }
    RECT SkipForwardRect() const {
        return HudButtonRect(kHudPad + (kButtonSize + 12) * 2 + kButtonSize / 2, kControlsCenterY, kButtonSize);
    }
    RECT TimeRect() const {
        const int left = kHudPad + (kButtonSize + 12) * 3 + 6;
        return RECT{left, kControlsCenterY - 14, left + 200, kControlsCenterY + 14};
    }
    RECT ExitRect() const {
        const int right = HudWidth() - kHudPad;
        return RECT{right - 168, kControlsCenterY - 20, right, kControlsCenterY + 20};
    }
    RECT MuteRect() const {
        const int right = ExitRect().left - 14;
        return HudButtonRect(right - kButtonSize / 2, kControlsCenterY, kButtonSize);
    }
    RECT VolumeTrackRect() const {
        const int right = MuteRect().left - 12;
        return RECT{right - 120, kControlsCenterY - 3, right, kControlsCenterY + 3};
    }

    HudItem HudItemAt(POINT point) const {
        // Sliders get a taller hit band than their visual track so they are
        // grabbable without pixel-hunting.
        const auto within = [&](RECT rect, int inflateY) {
            InflateRect(&rect, 0, inflateY);
            return PtInRect(&rect, point) != FALSE;
        };
        if (within(SeekTrackRect(), 12)) return HudItem::Seek;
        if (within(PlayRect(), 0)) return HudItem::PlayPause;
        if (within(SkipBackRect(), 0)) return HudItem::SkipBack;
        if (within(SkipForwardRect(), 0)) return HudItem::SkipForward;
        if (within(ExitRect(), 0)) return HudItem::Exit;
        if (within(MuteRect(), 0)) return HudItem::Mute;
        if (within(VolumeTrackRect(), 11)) return HudItem::Volume;
        return HudItem::None;
    }

    static std::wstring FormatClock(LONGLONG value100ns) {
        const LONGLONG seconds = (std::max<LONGLONG>)(0, value100ns / kOneSecond100ns);
        wchar_t buffer[32]{};
        if (seconds >= 3600) {
            swprintf_s(buffer, L"%lld:%02lld:%02lld", seconds / 3600, (seconds / 60) % 60, seconds % 60);
        } else {
            swprintf_s(buffer, L"%02lld:%02lld", seconds / 60, seconds % 60);
        }
        return buffer;
    }

    void DrawHudText(HDC dc, const std::wstring& text, RECT rect, COLORREF color, HFONT font, UINT flags) {
        HGDIOBJ oldFont = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, color);
        const std::wstring translated = ui::TranslateText(text);
        DrawTextW(dc, translated.c_str(), -1, &rect, flags | DT_NOPREFIX);
        SelectObject(dc, oldFont);
    }

    void DrawHudButton(HDC dc, RECT rect, UiIcon icon, bool hot, bool accent) {
        const COLORREF fill = accent ? (hot ? RGB(78, 128, 214) : RGB(56, 96, 168))
                                     : (hot ? RGB(52, 61, 77) : RGB(30, 36, 46));
        ui::FillRoundRect(dc, rect, 11, fill);
        const int cx = (rect.left + rect.right) / 2;
        const int cy = (rect.top + rect.bottom) / 2;
        DrawUiIcon(dc, icon, RECT{cx - 10, cy - 10, cx + 10, cy + 10},
                   hot || accent ? RGB(245, 249, 255) : RGB(206, 215, 230), 2);
    }

    void DrawHudSlider(HDC dc, RECT track, double filledRatio, bool hot, COLORREF fillColor) {
        const int radius = (track.bottom - track.top) / 2;
        ui::FillRoundRect(dc, track, radius, RGB(52, 59, 72));
        const int span = track.right - track.left;
        const int filled = (std::max)(0, (std::min)(span, static_cast<int>(span * filledRatio)));
        if (filled > 0) {
            ui::FillRoundRect(dc, RECT{track.left, track.top, track.left + filled, track.bottom}, radius, fillColor);
        }
        const int knobX = track.left + filled;
        const int knobY = (track.top + track.bottom) / 2;
        const int knobR = hot ? 9 : 7;
        ui::FillEllipse(dc, RECT{knobX - knobR, knobY - knobR, knobX + knobR, knobY + knobR}, RGB(238, 244, 253));
    }

    void PaintHud() {
        PAINTSTRUCT paint{};
        HDC target = BeginPaint(hud, &paint);
        RECT client{};
        GetClientRect(hud, &client);

        void* pixels = nullptr;
        HDC memory = CreateCompatibleDC(target);
        HBITMAP bitmap = memory ? ui::CreateLayeredDib(client.right, client.bottom, &pixels) : nullptr;
        if (memory && bitmap) {
            HGDIOBJ old = SelectObject(memory, bitmap);
            ui::FillRoundRect(memory, client, kHudRadius, RGB(15, 18, 24));
            ui::StrokeRoundRect(memory, client, kHudRadius, RGB(52, 60, 75));

            if (!hudFont) hudFont = ui::CreateThemeFont(14, FW_SEMIBOLD);
            if (!hudTimeFont) hudTimeFont = ui::CreateThemeFont(14, FW_NORMAL);

            const LONGLONG duration = DurationValue();
            const LONGLONG position = PositionValue();
            const double progress = duration > 0 ? static_cast<double>(position) / static_cast<double>(duration) : 0.0;
            DrawHudSlider(memory, SeekTrackRect(), progress,
                          hudHot == HudItem::Seek || hudDragging == HudItem::Seek, RGB(96, 152, 244));

            DrawHudButton(memory, PlayRect(), playing ? UiIcon::Pause : UiIcon::Play, hudHot == HudItem::PlayPause, true);
            DrawHudButton(memory, SkipBackRect(), UiIcon::SkipBack, hudHot == HudItem::SkipBack, false);
            DrawHudButton(memory, SkipForwardRect(), UiIcon::SkipForward, hudHot == HudItem::SkipForward, false);

            DrawHudText(memory, FormatClock(position) + L" / " + FormatClock(duration), TimeRect(),
                        RGB(180, 191, 209), hudTimeFont, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            const bool silent = muted || volume <= 0.0001f;
            DrawHudButton(memory, MuteRect(), silent ? UiIcon::Mute : UiIcon::Sound, hudHot == HudItem::Mute, false);
            DrawHudSlider(memory, VolumeTrackRect(), silent ? 0.0 : volume,
                          hudHot == HudItem::Volume || hudDragging == HudItem::Volume, RGB(126, 176, 244));

            const RECT exitRect = ExitRect();
            const bool exitHot = hudHot == HudItem::Exit;
            ui::FillRoundRect(memory, exitRect, 11, exitHot ? RGB(52, 61, 77) : RGB(30, 36, 46));
            ui::StrokeRoundRect(memory, exitRect, 11, exitHot ? RGB(110, 168, 255) : RGB(62, 71, 88));
            DrawUiIcon(memory, UiIcon::Collapse,
                       RECT{exitRect.left + 14, (exitRect.top + exitRect.bottom) / 2 - 9,
                            exitRect.left + 32, (exitRect.top + exitRect.bottom) / 2 + 9},
                       RGB(226, 233, 245), 2);
            DrawHudText(memory, L"Exit fullscreen",
                        RECT{exitRect.left + 40, exitRect.top, exitRect.right - 10, exitRect.bottom},
                        RGB(232, 238, 248), hudFont, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            const BYTE alpha = static_cast<BYTE>((std::clamp)(hudOpacity, 0.0f, 1.0f) * 240.0f);
            ui::PresentRoundedLayeredWindow(hud, memory, pixels, client.right, client.bottom, kHudRadius, alpha);
            SelectObject(memory, old);
        }
        if (bitmap) DeleteObject(bitmap);
        if (memory) DeleteDC(memory);
        EndPaint(hud, &paint);
    }

    void NoteHudActivity() {
        lastPointerActivity = GetTickCount64();
        if (!hudWanted) {
            hudWanted = true;
            ShowSystemCursor(true);
        }
    }

    void ShowSystemCursor(bool show) {
        // ShowCursor keeps a per-thread display counter, so every hide must be
        // matched exactly once - hence the flag rather than repeated calls.
        if (show == !cursorHidden) return;
        ShowCursor(show ? TRUE : FALSE);
        cursorHidden = !show;
    }

    void ApplySeekFromHud(int clientX) {
        const RECT track = SeekTrackRect();
        const int span = (std::max)(1, static_cast<int>(track.right - track.left));
        const double ratio = (std::clamp)(static_cast<double>(clientX - track.left) / span, 0.0, 1.0);
        const LONGLONG duration = DurationValue();
        if (duration > 0) SetPosition(static_cast<LONGLONG>(duration * ratio));
    }

    void ApplyVolumeFromHud(int clientX) {
        const RECT track = VolumeTrackRect();
        const int span = (std::max)(1, static_cast<int>(track.right - track.left));
        volume = static_cast<float>((std::clamp)(static_cast<double>(clientX - track.left) / span, 0.0, 1.0));
        if (volume > 0.0001f) muted = false;
        ApplyVolume();
    }

    void TickHud() {
        if (!hud || !fullscreen) return;
        POINT cursor{};
        GetCursorPos(&cursor);
        if (std::abs(cursor.x - lastPointerPos.x) > kPointerMoveThreshold ||
            std::abs(cursor.y - lastPointerPos.y) > kPointerMoveThreshold) {
            lastPointerPos = cursor;
            NoteHudActivity();
        }
        // A pointer resting on the HUD, or mid-drag on a slider, always counts as
        // active - otherwise the controls would fade out from under the cursor.
        const bool pointerOnHud = PtInRect(&hudRect, cursor) != FALSE;
        if (pointerOnHud || hudDragging != HudItem::None) NoteHudActivity();
        else if (hudWanted && GetTickCount64() - lastPointerActivity > kHudIdleTimeoutMs) hudWanted = false;

        const float goal = hudWanted ? 1.0f : 0.0f;
        if (hudOpacity != goal) {
            hudOpacity = goal > hudOpacity ? (std::min)(goal, hudOpacity + kHudFadeStep)
                                           : (std::max)(goal, hudOpacity - kHudFadeStep);
            if (hudOpacity <= 0.001f) {
                ShowWindow(hud, SW_HIDE);
                ShowSystemCursor(false);
            } else {
                if (!IsWindowVisible(hud)) ShowWindow(hud, SW_SHOWNOACTIVATE);
                InvalidateRect(hud, nullptr, FALSE);
            }
        } else if (hudWanted) {
            // Keep the clock and seek position live while the HUD is on screen.
            InvalidateRect(hud, nullptr, FALSE);
        }
    }

    LRESULT HandleHud(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
            case WM_PAINT: PaintHud(); return 0;
            case WM_ERASEBKGND: return 1;
            case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
            case WM_TIMER:
                if (wParam == kHudTimer) { TickHud(); return 0; }
                break;
            case WM_MOUSEMOVE: {
                const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                NoteHudActivity();
                if (hudDragging == HudItem::Seek) { ApplySeekFromHud(point.x); InvalidateRect(hud, nullptr, FALSE); return 0; }
                if (hudDragging == HudItem::Volume) { ApplyVolumeFromHud(point.x); InvalidateRect(hud, nullptr, FALSE); return 0; }
                const HudItem next = HudItemAt(point);
                if (next != hudHot) {
                    hudHot = next;
                    TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hud, 0};
                    TrackMouseEvent(&track);
                    InvalidateRect(hud, nullptr, FALSE);
                }
                return 0;
            }
            case WM_MOUSELEAVE:
                hudHot = HudItem::None;
                InvalidateRect(hud, nullptr, FALSE);
                return 0;
            case WM_LBUTTONDOWN: {
                const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                NoteHudActivity();
                const HudItem item = HudItemAt(point);
                if (item == HudItem::Seek || item == HudItem::Volume) {
                    hudDragging = item;
                    SetCapture(hud);
                    if (item == HudItem::Seek) ApplySeekFromHud(point.x);
                    else ApplyVolumeFromHud(point.x);
                    InvalidateRect(hud, nullptr, FALSE);
                }
                return 0;
            }
            case WM_LBUTTONUP: {
                const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                NoteHudActivity();
                if (hudDragging != HudItem::None) {
                    if (hudDragging == HudItem::Seek) ApplySeekFromHud(point.x);
                    else if (hudDragging == HudItem::Volume) ApplyVolumeFromHud(point.x);
                    hudDragging = HudItem::None;
                    ReleaseCapture();
                    InvalidateRect(hud, nullptr, FALSE);
                    return 0;
                }
                switch (HudItemAt(point)) {
                    case HudItem::PlayPause: TogglePlaybackInternal(); break;
                    case HudItem::SkipBack: SetPosition(PositionValue() - 10 * kOneSecond100ns); break;
                    case HudItem::SkipForward: SetPosition(PositionValue() + 10 * kOneSecond100ns); break;
                    case HudItem::Mute:
                        muted = !muted;
                        if (!muted && volume <= 0.0001f) volume = 0.5f;
                        ApplyVolume();
                        break;
                    case HudItem::Exit: RequestExitFullscreen(); return 0;
                    default: break;
                }
                if (parentEventWindow) PostMessageW(parentEventWindow, WM_APP_MEDIA_STATE_CHANGED, 0, 0);
                InvalidateRect(hud, nullptr, FALSE);
                return 0;
            }
            case WM_MOUSEWHEEL: {
                const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                volume = (std::clamp)(volume + (delta > 0 ? 0.05f : -0.05f), 0.0f, 1.0f);
                if (volume > 0.0001f) muted = false;
                ApplyVolume();
                NoteHudActivity();
                InvalidateRect(hud, nullptr, FALSE);
                return 0;
            }
            // Keys reach the video host, which owns focus; forward the ones the
            // HUD can receive when a click moved focus here.
            case WM_KEYDOWN:
                return Handle(message, wParam, lParam);
            default: break;
        }
        return DefWindowProcW(hud, message, wParam, lParam);
    }

    static LRESULT CALLBACK HudProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        Impl* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<Impl*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hud = window;
        }
        return self ? self->HandleHud(message, wParam, lParam)
                    : DefWindowProcW(window, message, wParam, lParam);
    }

    void CreateHud(const RECT& monitorRect) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = HudProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kFullscreenHudClass;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;

        const int monitorWidth = monitorRect.right - monitorRect.left;
        const int width = (std::min)(kHudMaxWidth, (std::max)(560, monitorWidth - kHudSideMargin * 2));
        const int left = monitorRect.left + (monitorWidth - width) / 2;
        const int top = monitorRect.bottom - kHudHeight - kHudBottomMargin;
        hudRect = RECT{left, top, left + width, top + kHudHeight};
        hudHot = HudItem::None;
        hudDragging = HudItem::None;
        hudOpacity = 1.0f;
        hudWanted = true;
        GetCursorPos(&lastPointerPos);
        lastPointerActivity = GetTickCount64();

        CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
                        kFullscreenHudClass, L"", WS_POPUP,
                        left, top, width, kHudHeight, nullptr, nullptr, instance, this);
        if (!hud) return;
        ShowWindow(hud, SW_SHOWNOACTIVATE);
        // Sit above the fullscreen video surface, which is itself topmost.
        SetWindowPos(hud, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        SetTimer(hud, kHudTimer, kHudFrameMs, nullptr);
        InvalidateRect(hud, nullptr, FALSE);
    }

    void DestroyHud() {
        ShowSystemCursor(true);
        if (hud && IsWindow(hud)) {
            KillTimer(hud, kHudTimer);
            DestroyWindow(hud);
        }
        hud = nullptr;
        hudRect = RECT{};
        hudHot = HudItem::None;
        hudDragging = HudItem::None;
        hudOpacity = 1.0f;
        hudWanted = true;
    }

    void SetFullscreen(bool enable, HWND ownerWindow) {
        if (!hwnd || fullscreen == enable) return;
        if (enable) {
            HMONITOR monitor = MonitorFromWindow(ownerWindow ? ownerWindow : hwnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO info{sizeof(info)};
            if (!GetMonitorInfoW(monitor, &info)) return;

            savedStyle = GetWindowLongPtrW(hwnd, GWL_STYLE);
            savedExStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
            SetParent(hwnd, nullptr);
            SetWindowLongPtrW(hwnd, GWL_STYLE, (savedStyle & ~static_cast<LONG_PTR>(WS_CHILD)) | WS_POPUP);
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE, savedExStyle | WS_EX_TOPMOST);
            SetWindowPos(hwnd, HWND_TOPMOST, info.rcMonitor.left, info.rcMonitor.top,
                         info.rcMonitor.right - info.rcMonitor.left,
                         info.rcMonitor.bottom - info.rcMonitor.top,
                         SWP_FRAMECHANGED | SWP_SHOWWINDOW);
            fullscreen = true;
            if (player) player->UpdateVideo();
            CreateHud(info.rcMonitor);
            // Take focus so Esc/F/Space reach the video surface directly.
            SetForegroundWindow(hwnd);
            SetFocus(hwnd);
        } else {
            DestroyHud();
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE, savedExStyle);
            SetWindowLongPtrW(hwnd, GWL_STYLE, savedStyle);
            SetParent(hwnd, parentEventWindow);
            SetWindowPos(hwnd, HWND_TOP, 0, 0, 1, 1, SWP_FRAMECHANGED | SWP_NOACTIVATE);
            fullscreen = false;
            if (parentEventWindow) SetForegroundWindow(parentEventWindow);
            if (player) player->UpdateVideo();
        }
    }

    bool CreateHost(HINSTANCE appInstance, HWND parent) {
        instance = appInstance;
        parentEventWindow = parent;
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kVideoHostClass;
        wc.style = CS_DBLCLKS;  // double-click leaves fullscreen
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
        hwnd = CreateWindowExW(0, kVideoHostClass, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 1, 1, parent, nullptr,
                               instance, this);
        return hwnd != nullptr;
    }

    bool LoadVideo(const std::wstring& path, std::wstring& error) {
        DestroyPlayer();
        status = L"Opening with Windows Media Foundation";
        if (!hwnd || FAILED(MFPCreateMediaPlayer(path.c_str(), TRUE, MFP_OPTION_NONE, this, hwnd, &player))) {
            status = L"Media Foundation could not open this video";
            error = status;
            return false;
        }
        player->SetBorderColor(RGB(7, 9, 12));
        player->SetAspectRatioMode(MFVideoARMode_PreservePicture);
        ApplyVolume();
        return true;
    }
};

MediaViewerWindow::MediaViewerWindow() : impl_(new Impl()) {}
MediaViewerWindow::~MediaViewerWindow() {
    if (impl_) {
        DestroyHost();
        impl_->Release();
        impl_ = nullptr;
    }
}

bool MediaViewerWindow::CreateHost(HINSTANCE instance, HWND parentEventWindow) {
    return impl_ && impl_->CreateHost(instance, parentEventWindow);
}

void MediaViewerWindow::DestroyHost() {
    if (!impl_) return;
    impl_->DestroyPlayer();
    impl_->DestroyHud();
    impl_->fullscreen = false;
    if (impl_->hudFont) { DeleteObject(impl_->hudFont); impl_->hudFont = nullptr; }
    if (impl_->hudTimeFont) { DeleteObject(impl_->hudTimeFont); impl_->hudTimeFont = nullptr; }
    if (impl_->hwnd && IsWindow(impl_->hwnd)) DestroyWindow(impl_->hwnd);
    impl_->hwnd = nullptr;
}

bool MediaViewerWindow::LoadVideo(const std::wstring& path, std::wstring& error) {
    return impl_ && impl_->LoadVideo(path, error);
}

void MediaViewerWindow::UnloadVideo() { if (impl_) impl_->DestroyPlayer(); }
bool MediaViewerWindow::HasVideo() const noexcept { return impl_ && impl_->player != nullptr; }

void MediaViewerWindow::SetBounds(const RECT& rect) {
    if (!impl_ || !impl_->hwnd) return;
    // While fullscreen the surface owns the whole monitor; the host's normal
    // in-window layout must not shrink it back.
    if (impl_->fullscreen) return;
    MoveWindow(impl_->hwnd, rect.left, rect.top, (std::max)(1L, rect.right - rect.left),
              (std::max)(1L, rect.bottom - rect.top), TRUE);
    if (impl_->player) impl_->player->UpdateVideo();
}

void MediaViewerWindow::SetFullscreen(bool enable, HWND ownerWindow) {
    if (impl_) impl_->SetFullscreen(enable, ownerWindow);
}

bool MediaViewerWindow::IsFullscreen() const noexcept { return impl_ && impl_->fullscreen; }

void MediaViewerWindow::SetVisible(bool visible) {
    if (!impl_ || !impl_->hwnd) return;
    ShowWindow(impl_->hwnd, visible ? SW_SHOWNA : SW_HIDE);
}

HWND MediaViewerWindow::Handle() const noexcept { return impl_ ? impl_->hwnd : nullptr; }

void MediaViewerWindow::TogglePlayback() {
    if (!impl_ || !impl_->player) return;
    MFP_MEDIAPLAYER_STATE state = MFP_MEDIAPLAYER_STATE_EMPTY;
    if (FAILED(impl_->player->GetState(&state))) return;
    if (state == MFP_MEDIAPLAYER_STATE_PLAYING) impl_->player->Pause();
    else impl_->player->Play();
}

void MediaViewerWindow::Play() { if (impl_ && impl_->player) impl_->player->Play(); }
void MediaViewerWindow::Pause() { if (impl_ && impl_->player) impl_->player->Pause(); }

void MediaViewerWindow::SkipBy(int deltaSeconds) {
    if (!impl_ || !impl_->player) return;
    impl_->SetPosition(impl_->PositionValue() + static_cast<LONGLONG>(deltaSeconds) * kOneSecond100ns);
}

void MediaViewerWindow::SetPositionRatio(double ratio) {
    if (!impl_ || !impl_->player) return;
    const LONGLONG duration = impl_->DurationValue();
    if (duration <= 0) return;
    ratio = (std::max)(0.0, (std::min)(1.0, ratio));
    impl_->SetPosition(static_cast<LONGLONG>(duration * ratio));
}

void MediaViewerWindow::SetVolume(float value) {
    if (!impl_) return;
    impl_->volume = (std::max)(0.0f, (std::min)(1.0f, value));
    if (impl_->volume > 0.0001f) impl_->muted = false;
    impl_->ApplyVolume();
}

void MediaViewerWindow::ToggleMute() {
    if (!impl_) return;
    impl_->muted = !impl_->muted;
    if (!impl_->muted && impl_->volume <= 0.0001f) impl_->volume = 0.5f;
    impl_->ApplyVolume();
}

bool MediaViewerWindow::IsPlaying() const noexcept { return impl_ && impl_->playing; }
bool MediaViewerWindow::IsMuted() const noexcept { return impl_ && impl_->muted; }
float MediaViewerWindow::Volume() const noexcept { return impl_ ? impl_->volume : 1.0f; }
LONGLONG MediaViewerWindow::PositionValue() const { return impl_ ? impl_->PositionValue() : 0; }
LONGLONG MediaViewerWindow::DurationValue() const { return impl_ ? impl_->DurationValue() : 0; }

const std::wstring& MediaViewerWindow::StatusText() const noexcept {
    static const std::wstring empty;
    return impl_ ? impl_->status : empty;
}
