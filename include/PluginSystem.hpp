#pragma once

#include "LunaScript.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace LunaPluginDispatchDetail {
// DispatchEvent is deliberately limited to the non-timer host events accepted
// by the parser. Exact, case-sensitive lookup both bounds cursor state and
// prevents a misspelled event from silently creating a second scheduling lane.
inline constexpr std::array<std::wstring_view, 23> kHostEventNames{
    L"app_started", L"capture_saved", L"recording_started", L"recording_saved",
    L"capture_failed", L"replay_started", L"replay_saved", L"replay_stopped",
    L"plugin_enabled", L"quick_panel_opened", L"quick_panel_closed", L"quick_panel_command",
    L"capture_queued", L"capture_cancelled", L"capture_copied", L"recording_stopped",
    L"recording_failed", L"recording_recovering", L"recording_recovered", L"replay_failed",
    L"replay_recovering", L"replay_recovered", L"plugins_reloaded",
};

inline constexpr std::optional<std::size_t> HostEventIndex(std::wstring_view eventName) noexcept {
    for (std::size_t index = 0; index < kHostEventNames.size(); ++index) {
        if (kHostEventNames[index] == eventName) return index;
    }
    return std::nullopt;
}

struct FairAtomicDispatchPlan {
    std::vector<std::size_t> handlerIndices;
    std::size_t nextCursor = 0;
};

// Pure selection seam shared by production dispatch and unit tests. Handler
// sizes are positive and individually bounded by the parser to 128, while the
// caller supplies the shared 256-instruction dispatch budget.
inline FairAtomicDispatchPlan SelectFairAtomicBatch(const std::vector<std::size_t>& handlerSizes,
                                                     std::size_t cursor,
                                                     std::size_t instructionBudget) {
    FairAtomicDispatchPlan plan;
    if (handlerSizes.empty()) return plan;

    const std::size_t start = cursor % handlerSizes.size();
    plan.handlerIndices.reserve(handlerSizes.size());
    for (std::size_t offset = 0; offset < handlerSizes.size(); ++offset) {
        const std::size_t index = (start + offset) % handlerSizes.size();
        // A handler is indivisible. Resume at this exact handler on the next
        // dispatch instead of repeatedly restarting at source-order index zero.
        if (handlerSizes[index] > instructionBudget) {
            plan.nextCursor = index;
            return plan;
        }
        instructionBudget -= handlerSizes[index];
        plan.handlerIndices.push_back(index);
    }
    // Even when every handler fits, rotate ordering deterministically so future
    // source edits or a newly larger handler cannot permanently favor index zero.
    plan.nextCursor = (start + 1) % handlerSizes.size();
    return plan;
}

class FairEventDispatchScheduler {
public:
    std::optional<FairAtomicDispatchPlan> Select(std::wstring_view eventName,
                                                 const std::vector<std::size_t>& handlerSizes,
                                                 std::size_t instructionBudget) {
        const auto eventIndex = HostEventIndex(eventName);
        if (!eventIndex) return std::nullopt;
        auto plan = SelectFairAtomicBatch(handlerSizes, cursors_[*eventIndex], instructionBudget);
        cursors_[*eventIndex] = plan.nextCursor;
        return plan;
    }

    void Reset() noexcept { cursors_.fill(0); }

private:
    std::array<std::size_t, kHostEventNames.size()> cursors_{};
};
}  // namespace LunaPluginDispatchDetail

struct LoadedPlugin {
    std::filesystem::path sourcePath;
    LunaPluginDefinition definition;
    std::vector<LunaDiagnostic> diagnostics;
    bool enabled = false;
    bool valid = false;
    // True when the currently parsed permission set has not been explicitly
    // approved. This includes first-time approval and a permission change made
    // while the plugin was enabled; callers can surface a review-required state.
    bool permissionsChanged = false;

    std::wstring DisplayName() const;
    std::wstring StatusText() const;
    std::wstring PermissionSummary() const;
};

struct PluginExecution {
    std::wstring pluginId;
    std::wstring pluginName;
    std::vector<LunaInstruction> instructions;
};

class PluginSystem {
public:
    bool Initialize();
    bool Reload();
    bool ReloadIfChanged();

    const std::vector<LoadedPlugin>& Plugins() const noexcept { return plugins_; }
    const LoadedPlugin* PluginAt(std::size_t index) const noexcept;
    LoadedPlugin* PluginAt(std::size_t index) noexcept;
    std::size_t EnabledCount() const noexcept;
    std::size_t ErrorCount() const noexcept;
    std::size_t ReviewCount() const noexcept;
    std::size_t CommandCount() const noexcept;

    bool SetEnabled(std::size_t index, bool enabled);
    bool ActivateTheme(std::size_t index);
    void ResetTheme();
    const LoadedPlugin* ActiveThemePlugin() const noexcept;
    const LunaThemeDefinition* ActiveTheme() const noexcept;
    const std::wstring& ActiveThemeId() const noexcept { return activeThemeId_; }
    bool ActivateLocale(std::size_t index);
    void ResetLocale();
    const LoadedPlugin* ActiveLocalePlugin() const noexcept;
    const LunaLocaleDefinition* ActiveLocale() const noexcept;
    const std::wstring& ActiveLocaleId() const noexcept { return activeLocaleId_; }

    PluginExecution ExecuteCommand(std::size_t pluginIndex, std::size_t commandIndex) const;
    PluginExecution ExecuteCommand(const std::wstring& pluginId, const std::wstring& commandId) const;
    std::vector<PluginExecution> DispatchEvent(const std::wstring& eventName);
    // Call roughly once a second from the host's background tick (not gated on the
    // main window being foreground) to fire any "on timer every N { ... }" handlers
    // that are due. Owns its own per-timer scheduling state.
    std::vector<PluginExecution> DispatchDueTimers(std::uint64_t nowMs);

    const std::filesystem::path& RootDirectory() const noexcept { return rootDirectory_; }
    const std::filesystem::path& DocumentationPath() const noexcept { return documentationPath_; }
    std::filesystem::path PluginDirectory(std::size_t index) const;
    const std::wstring& LastReloadSummary() const noexcept { return lastReloadSummary_; }

private:
    std::wstring ReadUtf8File(const std::filesystem::path& path, bool& ok) const;
    std::wstring ComputeDirectoryStamp() const;
    bool ReadEnabledState(const std::wstring& pluginId) const;
    void WriteEnabledState(const std::wstring& pluginId, bool enabled) const;
    std::wstring ReadPermissionFingerprint(const std::wstring& pluginId) const;
    void WritePermissionFingerprint(const std::wstring& pluginId, const std::wstring& fingerprint) const;
    void ReadHostState();
    void WriteActiveTheme() const;
    void WriteActiveLocale() const;
    void LocateDocumentation();

    std::filesystem::path rootDirectory_;
    std::filesystem::path statePath_;
    std::filesystem::path documentationPath_;
    std::vector<LoadedPlugin> plugins_;
    std::wstring activeThemeId_;
    std::wstring activeLocaleId_;
    std::wstring directoryStamp_;
    std::wstring lastReloadSummary_;
    std::unordered_map<std::wstring, std::uint64_t> timerNextFireMs_;
    LunaPluginDispatchDetail::FairEventDispatchScheduler eventDispatchScheduler_;
    std::size_t timerDispatchCursor_ = 0;
};
