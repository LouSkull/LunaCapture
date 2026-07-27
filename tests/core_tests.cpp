#include "CoreUtils.hpp"
#include "Localization.hpp"
#include "LunaScript.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>

int main() {
    InteractiveCaptureGate captureGate;
    if (!captureGate.TryBegin() || captureGate.TryBegin() || !captureGate.IsActive()) return 1;
    captureGate.End();
    if (captureGate.IsActive() || !captureGate.TryBegin()) return 4;
    captureGate.End();

    PluginFeedbackCircuitBreaker feedbackBreaker;
    for (std::size_t index = 0; index < PluginFeedbackCircuitBreaker::kBurstLimit; ++index) {
        if (!feedbackBreaker.TryAcquire(static_cast<std::uint64_t>(index) * 1000)) return 63;
    }
    if (feedbackBreaker.TryAcquire(8000) || !feedbackBreaker.IsTripped() ||
        feedbackBreaker.TryAcquire(120000)) return 64;
    PluginFeedbackCircuitBreaker pacedFeedback;
    for (std::size_t index = 0; index < PluginFeedbackCircuitBreaker::kBurstLimit * 2; ++index) {
        if (!pacedFeedback.TryAcquire(static_cast<std::uint64_t>(index) *
                                      PluginFeedbackCircuitBreaker::kWindowMilliseconds)) return 65;
    }
    if (pacedFeedback.IsTripped()) return 66;

    constexpr std::uint32_t recorderWidth = 1280;
    constexpr std::uint32_t recorderHeight = 720;
    constexpr std::uint32_t desktopWidth = 1920;
    constexpr std::uint32_t desktopHeight = 1080;
    std::vector<std::uint8_t> recorderFrame(static_cast<std::size_t>(recorderWidth) * recorderHeight * 4, 0);
    std::vector<std::uint8_t> desktopFrame(static_cast<std::size_t>(desktopWidth) * desktopHeight * 4, 0);
    const auto setPixel = [&](std::uint32_t x, std::uint32_t y, std::uint8_t value) {
        auto* pixel = recorderFrame.data() + (static_cast<std::size_t>(y) * recorderWidth + x) * 4;
        pixel[0] = value;
        pixel[1] = static_cast<std::uint8_t>(value + 1);
        pixel[2] = static_cast<std::uint8_t>(value + 2);
        pixel[3] = 19;
    };
    setPixel(0, 0, 10);
    setPixel(640, 360, 40);
    setPixel(1279, 719, 70);
    if (!BlitBgraScaledNearest(recorderFrame.data(), recorderWidth, recorderHeight, recorderWidth * 4,
                               desktopFrame.data(), desktopWidth, desktopHeight, desktopWidth * 4)) return 5;
    const auto matchesPixel = [&](std::uint32_t x, std::uint32_t y, std::uint8_t value) {
        const auto* pixel = desktopFrame.data() + (static_cast<std::size_t>(y) * desktopWidth + x) * 4;
        return pixel[0] == value && pixel[1] == value + 1 && pixel[2] == value + 2 && pixel[3] == 255;
    };
    if (!matchesPixel(0, 0, 10) || !matchesPixel(960, 540, 40) ||
        !matchesPixel(1919, 1079, 70)) return 6;

    if (MakeScreenshotFilename(L"Discord", 2026, 7, 21, 9, 5, 4, 7, false) != L"2026-07-21_09-05-04_007_Discord.png") return 10;
    if (MakeScreenshotFilename(L"Grand Theft Auto V", 2026, 7, 21, 9, 5, 4, 7, true) != L"2026-07-21_09-05-04_007_Grand Theft Auto V.jpg") return 11;
    if (SanitizeFilenamePrefix(L"  Discord: Friends / Chat  ") != L"Discord_ Friends _ Chat") return 12;
    if (SanitizeFilenamePrefix(L"...   ") != L"Desktop") return 13;
    if (FileNameOnly(L"C:\\Shots\\a.png") != L"a.png") return 14;
    if (EllipsizeMiddle(L"1234567890", 7) != L"123…890") return 15;
    if (std::abs(QpcMilliseconds(100, 350, 1000) - 250.0) >= 0.0001) return 16;
    if (NormalizeAacBitrateKbps(64) != 96 || NormalizeAacBitrateKbps(127) != 128 ||
        NormalizeAacBitrateKbps(176) != 160 || NormalizeAacBitrateKbps(256) != 192) return 17;
    std::wstring localized;
    if (!HasLocalizationPlaceholders(L"Saved {0} of {1}") ||
        !ExpandLocalizationTemplate(L"Saved {0} of {1}", L"{1} total, {0} saved", L"Saved 3 of 8", localized) ||
        localized != L"8 total, 3 saved") return 18;
    if (ExpandLocalizationTemplate(L"Monitor {0}", L"Monitor {0}", L"Display 2", localized)) return 19;
    std::vector<std::uint8_t> recoveryFrame(64 * 36 * 4);
    if (!FillCaptureRecoveryFrame(recoveryFrame.data(), 64, 36, 64 * 4)) return 30;
    bool recoveryFrameHasVariation = false;
    for (std::size_t pixel = 0; pixel < recoveryFrame.size(); pixel += 4) {
        if (recoveryFrame[pixel + 3] != 255) return 31;
        if (pixel >= 4 && recoveryFrame[pixel] != recoveryFrame[0]) recoveryFrameHasVariation = true;
    }
    if (!recoveryFrameHasVariation) return 32;
    const auto chronologicalFirst = std::filesystem::path(
        MakeChronologicalMediaPath(L"chronology-test", L"Zulu", L".png")).filename().wstring();
    const auto chronologicalSecond = std::filesystem::path(
        MakeChronologicalMediaPath(L"chronology-test", L"Alpha", L".png")).filename().wstring();
    if (chronologicalFirst >= chronologicalSecond || chronologicalFirst.ends_with(L"_Zulu.png") == false ||
        chronologicalSecond.ends_with(L"_Alpha.png") == false) return 33;
    if (!HasChronologicalMediaFilename(chronologicalFirst) ||
        HasChronologicalMediaFilename(L"Screenshot_2026-07-21_09-05-04_007.png")) return 34;
    const std::uint8_t sourcePixels[] = {
        1, 2, 3, 0,  4, 5, 6, 0,
        7, 8, 9, 0,  10, 11, 12, 0,
    };
    std::vector<std::uint8_t> scaled;
    std::uint32_t scaledWidth = 0;
    std::uint32_t scaledHeight = 0;
    std::uint32_t scaledStride = 0;
    BgraPreviewScaler scaler;
    if (!scaler.Scale(sourcePixels, 2, 2, 8, 1, 1, scaled,
                      scaledWidth, scaledHeight, scaledStride) ||
        scaledWidth != 1 || scaledHeight != 1 || scaledStride != 4 ||
        scaled != std::vector<std::uint8_t>{1, 2, 3, 255}) return 2;

    std::vector<std::uint8_t> fullHd(static_cast<std::size_t>(1920) * 1080 * 4, 127);
    const auto benchmarkStart = std::chrono::steady_clock::now();
    for (int frame = 0; frame < 120; ++frame) {
        if (!scaler.Scale(fullHd.data(), 1920, 1080, 1920 * 4, 640, 360, scaled,
                          scaledWidth, scaledHeight, scaledStride)) return 3;
    }
    const auto benchmarkMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - benchmarkStart).count();
    std::cout << "Preview scaler: 120 frames in " << benchmarkMs << " ms\n";

    const std::wstring pluginSource = LR"LUNA(
        luna 1;
        plugin "test.theme" {
            name "Test Theme";
            version "1.0.0";
            author "Tests";
            description "Parser smoke test";
            permission theme;
            permission notifications;
            permission audio;
            permission settings;
            permission localization;
            theme "Test" {
                background #080A10; background_secondary #17102A; accent #7C5CFF;
                corner_scale 120; backdrop grid; backdrop_opacity 30; backdrop_scale 40;
                background_image "art.png"; background_image_mode contain; background_image_opacity 20;
            }
            locale "Test language" { "Home" = "Start"; "Settings" = "Options"; }
            command "hello" {
                title "Hello";
                description "Shows a notification";
                icon "spark";
                surface quick_panel;
                surface tray;
                order 25;
                run {
                    audio add_microphone "default";
                    audio noise_suppression "default" 70;
                    setting recording_fps 120;
                    setting preview on;
                    notify "LunaScript" "Command executed";
                }
            }
            on app_started { notify "Loaded" "Event executed"; }
        }
    )LUNA";
    LunaPluginDefinition plugin;
    std::vector<LunaDiagnostic> diagnostics;
    if (!LunaScript::Parse(pluginSource, plugin, diagnostics) || !diagnostics.empty()) return 20;
    if (plugin.requiredHostApi != 4 || plugin.hostApiRequirementPresent ||
        plugin.id != L"test.theme" || plugin.commands.size() != 1 || plugin.events.size() != 1 ||
        !plugin.theme.present || plugin.theme.cornerScalePercent != 120 ||
        plugin.theme.backdropPattern != LunaBackdropPattern::Grid || plugin.theme.backdropScale != 40 ||
        plugin.theme.backgroundImageMode != LunaBackgroundImageMode::Contain ||
        !plugin.locale.present || plugin.locale.translations.size() != 2 || plugin.commands[0].instructions.size() != 5 ||
        plugin.commands[0].order != 25 ||
        !LunaScript::HasCommandSurface(plugin.commands[0], LunaCommandSurface::QuickPanel) ||
        !LunaScript::HasCommandSurface(plugin.commands[0], LunaCommandSurface::TrayMenu) ||
        !LunaScript::HasPermission(plugin, LunaPermission::Notifications)) return 21;
    const std::wstring denied = LR"LUNA(
        luna 1; plugin "test.denied" {
            name "Denied"; version "1"; author "Tests";
            command "capture" { title "Capture"; run { capture active; } }
        }
    )LUNA";
    diagnostics.clear();
    if (LunaScript::Parse(denied, plugin, diagnostics) || diagnostics.empty()) return 22;

    const std::wstring timerSource = LR"LUNA(
        luna 1; plugin "test.timer" {
            name "Timer"; version "1"; author "Tests";
            permission capture;
            on timer every 900 { capture active; }
        }
    )LUNA";
    diagnostics.clear();
    if (!LunaScript::Parse(timerSource, plugin, diagnostics) || !diagnostics.empty()) return 25;
    if (plugin.events.size() != 1 || plugin.events[0].eventName != L"timer" ||
        plugin.events[0].intervalSeconds != 900) return 26;
    const std::wstring badTimerSource = LR"LUNA(
        luna 1; plugin "test.timer-bad" {
            name "Timer"; version "1"; author "Tests";
            permission capture;
            on timer every 2 { capture active; }
        }
    )LUNA";
    diagnostics.clear();
    if (LunaScript::Parse(badTimerSource, plugin, diagnostics) || diagnostics.empty()) return 27;

    const std::wstring lifecycleSource = LR"LUNA(
        luna 1; plugin "test.lifecycle" {
            name "Lifecycle"; version "1"; author "Tests";
            permission recording; permission replay;
            command "safe-start" {
                title "Safe start"; surface tray; order 0;
                run {
                    recording start; recording stop;
                    replay start; replay save_monitor 2; replay save_all; replay stop;
                }
            }
        }
    )LUNA";
    diagnostics.clear();
    if (!LunaScript::Parse(lifecycleSource, plugin, diagnostics) || !diagnostics.empty()) return 40;
    if (plugin.commands.size() != 1 || plugin.commands[0].instructions.size() != 6 ||
        plugin.commands[0].instructions[0].type != LunaInstructionType::RecordingStart ||
        plugin.commands[0].instructions[3].type != LunaInstructionType::ReplaySaveMonitor ||
        plugin.commands[0].instructions[3].value != 2 ||
        plugin.commands[0].instructions[5].type != LunaInstructionType::ReplayStop) return 41;

    const std::wstring badOrderSource = LR"LUNA(
        luna 1; plugin "test.order-bad" {
            name "Order"; version "1"; author "Tests";
            command "bad" { title "Bad"; order -1; run { } }
        }
    )LUNA";
    diagnostics.clear();
    if (LunaScript::Parse(badOrderSource, plugin, diagnostics) || diagnostics.empty()) return 42;

    const std::wstring badLocaleTemplate = LR"LUNA(
        luna 1; plugin "test.locale-bad" {
            name "Locale"; version "1"; author "Tests";
            permission localization;
            locale "Bad" { "Monitor {0}" = "Monitor {1}"; }
        }
    )LUNA";
    diagnostics.clear();
    if (LunaScript::Parse(badLocaleTemplate, plugin, diagnostics) || diagnostics.empty()) return 28;

    const std::wstring hostFiveSource = LR"LUNA(
        luna 1;
        plugin "test.host-five" {
            requires host_api 5;
            name "Host Five"; version "1"; author "Tests";
            permission theme; permission localization; permission capture;
            permission settings; permission open_folders; permission ui;
            theme "Everything" {
                appearance light;
                sidebar #EEF2F8; topbar #FAFBFD;
                surface_hover #EAF0F8; surface_pressed #DDE7F4; surface_selected #D5E6FC;
                input #FFFFFF; border_hover #8899B2; focus #276ED0;
                text_secondary #445066; disabled #929CAD; text_on_accent #FFFFFF;
                accent_pressed #2459A8; accent_secondary #8855D8; info #1788B9; shadow #202838;
                border_scale 135; corner_scale 120;
                font_family "Segoe UI Variable"; font_scale 110; font_weight 500; font_italic off;
                saturation 105; contrast 110; brightness 100; motion_scale 80;
                backdrop hexagons; backdrop_scope all; backdrop_color #367DD8;
                backdrop_color_secondary #A05CE4; backdrop_opacity 32; backdrop_scale 42;
                backdrop_thickness 2; backdrop_speed 25; backdrop_seed 77;
                gradient_style radial; gradient_angle 210; gradient_center_x 35; gradient_center_y 42;
                gradient_spread 125; vignette_color #142035; vignette_opacity 18; noise_opacity 4;
                background_image "art/background.png"; background_image_mode stretch;
                background_image_opacity 44; background_image_position_x 25; background_image_position_y 75;
                background_image_scale 125; background_image_tint #3366AA;
                background_image_tint_opacity 15; background_image_brightness 115;
                background_image_saturation 90;
            }
            command "host-five" {
                title "Host Five";
                run {
                    capture monitor 2;
                    setting image_format jpeg; setting jpeg_quality 88;
                    setting output_width 2560; setting output_height 1440;
                    setting copy_region on; setting overlay_duration 1800;
                    setting replay_preview off; setting system_audio on;
                    setting system_audio_volume 125; setting application_audio off;
                    setting application_audio_volume 80; setting audio_bitrate 160;
                    ui page plugins; ui show; ui hide; ui toggle;
                    quick_panel show; quick_panel hide; quick_panel toggle;
                    theme activate; theme reset; locale activate; locale reset;
                    open replay_cache; open logs;
                }
            }
        }
    )LUNA";
    diagnostics.clear();
    if (!LunaScript::Parse(hostFiveSource, plugin, diagnostics) || !diagnostics.empty()) return 50;
    if (plugin.requiredHostApi != 5 || !plugin.hostApiRequirementPresent ||
        plugin.theme.appearance != LunaThemeAppearance::Light ||
        plugin.theme.backdropPattern != LunaBackdropPattern::Hexagons ||
        plugin.theme.gradientStyle != LunaGradientStyle::Radial ||
        plugin.theme.backdropScope != LunaBackdropScope::All ||
        plugin.theme.backgroundImageMode != LunaBackgroundImageMode::Stretch ||
        plugin.theme.fontFamily != L"Segoe UI Variable" || plugin.theme.fontScalePercent != 110 ||
        plugin.theme.borderScalePercent != 135 || plugin.theme.backdropSpeedPercent != 25 ||
        plugin.theme.backgroundImageTintOpacityPercent != 15 || plugin.commands.size() != 1 ||
        plugin.commands[0].instructions.size() != 26 ||
        !LunaScript::HasPermission(plugin, LunaPermission::Ui)) return 51;
    constexpr std::array<LunaInstructionType, 26> hostFiveInstructionTypes{
        LunaInstructionType::CaptureMonitor,
        LunaInstructionType::SettingImageFormat,
        LunaInstructionType::SettingJpegQuality,
        LunaInstructionType::SettingOutputWidth,
        LunaInstructionType::SettingOutputHeight,
        LunaInstructionType::SettingCopyRegion,
        LunaInstructionType::SettingOverlayDuration,
        LunaInstructionType::SettingReplayPreview,
        LunaInstructionType::SettingSystemAudio,
        LunaInstructionType::SettingSystemAudioVolume,
        LunaInstructionType::SettingApplicationAudio,
        LunaInstructionType::SettingApplicationAudioVolume,
        LunaInstructionType::SettingAudioBitrate,
        LunaInstructionType::UiNavigate,
        LunaInstructionType::UiShow,
        LunaInstructionType::UiHide,
        LunaInstructionType::UiToggle,
        LunaInstructionType::QuickPanelShow,
        LunaInstructionType::QuickPanelHide,
        LunaInstructionType::QuickPanelToggle,
        LunaInstructionType::ThemeActivate,
        LunaInstructionType::ThemeReset,
        LunaInstructionType::LocaleActivate,
        LunaInstructionType::LocaleReset,
        LunaInstructionType::OpenReplayCache,
        LunaInstructionType::OpenLogs,
    };
    for (std::size_t index = 0; index < hostFiveInstructionTypes.size(); ++index) {
        if (plugin.commands[0].instructions[index].type != hostFiveInstructionTypes[index]) return 61;
    }
    if (plugin.commands[0].instructions[0].value != 2 ||
        plugin.commands[0].instructions[1].value != 1 ||
        plugin.commands[0].instructions[2].value != 88 ||
        plugin.commands[0].instructions[3].value != 2560 ||
        plugin.commands[0].instructions[4].value != 1440 ||
        plugin.commands[0].instructions[5].value != 1 ||
        plugin.commands[0].instructions[6].value != 1800 ||
        plugin.commands[0].instructions[7].value != 0 ||
        plugin.commands[0].instructions[8].value != 1 ||
        plugin.commands[0].instructions[9].value != 125 ||
        plugin.commands[0].instructions[10].value != 0 ||
        plugin.commands[0].instructions[11].value != 80 ||
        plugin.commands[0].instructions[12].value != 160 ||
        plugin.commands[0].instructions[13].first != L"plugins") return 62;

    const std::wstring futureHost = LR"LUNA(
        luna 1; plugin "test.future" {
            requires host_api 999; name "Future"; version "1"; author "Tests";
        }
    )LUNA";
    diagnostics.clear();
    if (LunaScript::Parse(futureHost, plugin, diagnostics) || diagnostics.empty()) return 52;

    const std::wstring duplicateThemeKey = LR"LUNA(
        luna 1; plugin "test.duplicate-theme" {
            name "Duplicate"; version "1"; author "Tests"; permission theme;
            theme "Bad" { accent #112233; accent #445566; }
        }
    )LUNA";
    diagnostics.clear();
    if (LunaScript::Parse(duplicateThemeKey, plugin, diagnostics) || diagnostics.empty()) return 53;

    const std::wstring invalidThemeRange = LR"LUNA(
        luna 1; plugin "test.theme-range" {
            name "Range"; version "1"; author "Tests"; permission theme;
            theme "Bad" { backdrop_opacity 101; }
        }
    )LUNA";
    diagnostics.clear();
    if (LunaScript::Parse(invalidThemeRange, plugin, diagnostics) || diagnostics.empty()) return 54;

    const std::wstring missingUiPermission = LR"LUNA(
        luna 1; plugin "test.no-ui" {
            name "No UI"; version "1"; author "Tests";
            command "open" { title "Open"; run { ui page plugins; } }
        }
    )LUNA";
    diagnostics.clear();
    if (LunaScript::Parse(missingUiPermission, plugin, diagnostics) || diagnostics.empty()) return 55;

    const std::wstring missingRun = LR"LUNA(
        luna 1; plugin "test.no-run" {
            name "No run"; version "1"; author "Tests";
            command "empty" { title "Empty"; }
        }
    )LUNA";
    diagnostics.clear();
    if (LunaScript::Parse(missingRun, plugin, diagnostics) || diagnostics.empty()) return 56;

    const std::wstring omittedHostFiveRequirement = LR"LUNA(
        luna 1; plugin "test.missing-host-five" {
            name "Missing host five"; version "1"; author "Tests";
            permission ui;
            command "show" { title "Show"; run { ui show; } }
        }
    )LUNA";
    diagnostics.clear();
    if (LunaScript::Parse(omittedHostFiveRequirement, plugin, diagnostics) || diagnostics.empty()) return 57;

    const std::wstring explicitOldHostWithNewSyntax = LR"LUNA(
        luna 1; plugin "test.old-host-new-syntax" {
            requires host_api 4;
            name "Old host"; version "1"; author "Tests"; permission theme;
            theme "Invalid API mix" { appearance amoled; }
        }
    )LUNA";
    diagnostics.clear();
    if (LunaScript::Parse(explicitOldHostWithNewSyntax, plugin, diagnostics) || diagnostics.empty()) return 58;

    const std::wstring explicitHostOne = LR"LUNA(
        luna 1; plugin "test.host-one" {
            requires host_api 1;
            name "Host one"; version "1"; author "Tests";
        }
    )LUNA";
    diagnostics.clear();
    if (!LunaScript::Parse(explicitHostOne, plugin, diagnostics) || !diagnostics.empty() ||
        plugin.requiredHostApi != 1 || !plugin.hostApiRequirementPresent) return 59;

    const std::wstring multilineLog = LR"LUNA(
        luna 1; plugin "test.log-injection" {
            name "Log injection"; version "1"; author "Tests"; permission diagnostics;
            command "forge" { title "Forge"; run { log "first\n[2099-01-01] forged"; } }
        }
    )LUNA";
    diagnostics.clear();
    if (LunaScript::Parse(multilineLog, plugin, diagnostics) || diagnostics.empty()) return 60;

    std::cout << "Core tests passed\n";
    return 0;
}
