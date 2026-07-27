#include "PluginSystem.hpp"

#include "WinUtil.hpp"

#include <Windows.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <unordered_map>

namespace {
constexpr std::size_t kMaximumPlugins = 128;
constexpr std::size_t kDispatchInstructionBudget = 256;

bool IsLunaSource(const std::filesystem::path& path) {
    return _wcsicmp(path.extension().c_str(), L".luna") == 0;
}

std::wstring CanonicalPluginId(const std::wstring& value) {
    if (value.empty()) return {};
    const int length = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, value.data(),
                                     static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr, 0);
    if (length <= 0) return value;
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, value.data(),
                      static_cast<int>(value.size()), result.data(), length,
                      nullptr, nullptr, 0) != length) return value;
    return result;
}

bool SamePluginId(const std::wstring& left, const std::wstring& right) {
    return CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()),
                                right.c_str(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

std::wstring PermissionFingerprint(const LunaPluginDefinition& definition) {
    std::vector<std::wstring> names;
    names.reserve(definition.permissions.size());
    for (const LunaPermission permission : definition.permissions) {
        names.emplace_back(LunaScript::PermissionName(permission));
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());

    // Store the normalized capability list rather than relying on enum values or
    // source order. The version prefix leaves room to change the representation
    // without accidentally accepting an approval produced by another scheme.
    std::wstring fingerprint = L"v1";
    for (const auto& name : names) {
        fingerprint += L'|';
        fingerprint += name;
    }
    return fingerprint;
}

std::optional<std::filesystem::path> ThemeBackgroundAsset(const LoadedPlugin& plugin) {
    if (!plugin.valid || !plugin.definition.theme.present || plugin.definition.theme.backgroundImage.empty()) {
        return std::nullopt;
    }

    const std::filesystem::path relative(plugin.definition.theme.backgroundImage);
    if (relative.empty() || relative.is_absolute() || relative.has_root_path()) return std::nullopt;
    for (const auto& component : relative) {
        if (component == L"..") return std::nullopt;
    }

    const std::wstring extension = relative.extension().wstring();
    if (_wcsicmp(extension.c_str(), L".png") != 0 && _wcsicmp(extension.c_str(), L".jpg") != 0 &&
        _wcsicmp(extension.c_str(), L".jpeg") != 0 && _wcsicmp(extension.c_str(), L".bmp") != 0) {
        return std::nullopt;
    }

    std::error_code error;
    const auto pluginRoot = std::filesystem::weakly_canonical(plugin.sourcePath.parent_path(), error);
    if (error) return std::nullopt;
    const auto asset = std::filesystem::weakly_canonical(pluginRoot / relative, error);
    if (error) return std::nullopt;
    const auto withinPlugin = asset.lexically_relative(pluginRoot);
    if (withinPlugin.empty() || withinPlugin.is_absolute()) return std::nullopt;
    for (const auto& component : withinPlugin) {
        if (component == L"..") return std::nullopt;
    }
    return asset;
}

std::wstring Utf8ToWide(const std::string& value, bool& ok) {
    ok = false;
    if (value.empty()) { ok = true; return {}; }
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                            result.data(), count) != count) return {};
    ok = true;
    return result;
}

std::wstring DiagnosticText(const LunaDiagnostic& diagnostic) {
    return L"Line " + std::to_wstring(diagnostic.line) + L":" + std::to_wstring(diagnostic.column) +
           L" · " + diagnostic.message;
}
}  // namespace

std::wstring LoadedPlugin::DisplayName() const {
    if (!definition.name.empty()) return definition.name;
    if (!definition.id.empty()) return definition.id;
    return sourcePath.filename().wstring();
}

std::wstring LoadedPlugin::StatusText() const {
    if (!valid) return diagnostics.empty() ? L"Invalid LunaScript" : DiagnosticText(diagnostics.front());
    if (permissionsChanged) return L"Review required · permissions changed or not yet approved";
    return enabled ? L"Enabled · sandbox active" : L"Disabled · no actions execute";
}

std::wstring LoadedPlugin::PermissionSummary() const {
    if (definition.permissions.empty()) return L"No permissions";
    std::wstring result;
    for (LunaPermission permission : definition.permissions) {
        if (!result.empty()) result += L", ";
        result += LunaScript::PermissionName(permission);
    }
    return result;
}

bool PluginSystem::Initialize() {
    rootDirectory_ = std::filesystem::path(GetLocalAppDataDirectory()) / L"Plugins";
    statePath_ = std::filesystem::path(GetLocalAppDataDirectory()) / L"plugins.ini";
    if (!EnsureDirectoryExists(rootDirectory_.wstring())) return false;
    ReadHostState();
    LocateDocumentation();
    return Reload();
}

bool PluginSystem::Reload() {
    std::vector<LoadedPlugin> loaded;
    std::error_code error;
    if (!std::filesystem::exists(rootDirectory_, error)) EnsureDirectoryExists(rootDirectory_.wstring());
    std::vector<std::filesystem::path> sources;
    for (std::filesystem::recursive_directory_iterator iterator(rootDirectory_,
             std::filesystem::directory_options::skip_permission_denied, error), end;
         !error && iterator != end; iterator.increment(error)) {
        if (!iterator->is_regular_file(error)) continue;
        if (IsLunaSource(iterator->path())) sources.push_back(iterator->path());
    }
    std::sort(sources.begin(), sources.end());
    if (sources.size() > kMaximumPlugins) sources.resize(kMaximumPlugins);
    std::unordered_map<std::wstring, std::size_t> ids;
    for (const auto& source : sources) {
        LoadedPlugin plugin;
        plugin.sourcePath = source;
        bool read = false;
        const std::wstring text = ReadUtf8File(source, read);
        if (!read) {
            plugin.diagnostics.push_back({1, 1, L"Source must be a readable UTF-8 file"});
        } else {
            plugin.valid = LunaScript::Parse(text, plugin.definition, plugin.diagnostics);
        }
        if (plugin.valid) {
            const std::wstring canonicalId = CanonicalPluginId(plugin.definition.id);
            const auto duplicate = ids.find(canonicalId);
            if (duplicate != ids.end()) {
                plugin.valid = false;
                plugin.diagnostics.push_back({1, 1, L"Duplicate plugin id: " + plugin.definition.id});
                loaded[duplicate->second].valid = false;
                loaded[duplicate->second].diagnostics.push_back({1, 1, L"Duplicate plugin id: " + plugin.definition.id});
            } else {
                ids.emplace(canonicalId, loaded.size());
                const bool wasEnabled = ReadEnabledState(plugin.definition.id);
                const std::wstring currentFingerprint = PermissionFingerprint(plugin.definition);
                const std::wstring approvedFingerprint = ReadPermissionFingerprint(plugin.definition.id);
                plugin.permissionsChanged = approvedFingerprint != currentFingerprint;
                plugin.enabled = wasEnabled && !plugin.permissionsChanged;
                if (wasEnabled && plugin.permissionsChanged) {
                    // Persist the fail-closed transition. A subsequent reload must
                    // not re-enable changed capabilities before the user reviews
                    // them and SetEnabled(true) records the new fingerprint.
                    WriteEnabledState(plugin.definition.id, false);
                }
            }
        }
        loaded.push_back(std::move(plugin));
    }
    plugins_ = std::move(loaded);
    // A reload can rewrite event bodies/intervals in place, so any in-flight timer
    // countdowns are stale and should restart clean from the freshly parsed source.
    timerNextFireMs_.clear();
    eventDispatchScheduler_.Reset();
    timerDispatchCursor_ = 0;
    if (!activeThemeId_.empty() && !ActiveThemePlugin()) {
        activeThemeId_.clear();
        WriteActiveTheme();
    }
    if (!activeLocaleId_.empty() && !ActiveLocalePlugin()) {
        activeLocaleId_.clear();
        WriteActiveLocale();
    }
    directoryStamp_ = ComputeDirectoryStamp();
    lastReloadSummary_ = std::to_wstring(plugins_.size()) + L" plugin(s) · " +
                         std::to_wstring(EnabledCount()) + L" enabled · " +
                         std::to_wstring(ReviewCount()) + L" review · " +
                         std::to_wstring(ErrorCount()) + L" error(s)";
    return true;
}

bool PluginSystem::ReloadIfChanged() {
    const std::wstring current = ComputeDirectoryStamp();
    if (current == directoryStamp_) return false;
    Reload();
    return true;
}

const LoadedPlugin* PluginSystem::PluginAt(std::size_t index) const noexcept {
    return index < plugins_.size() ? &plugins_[index] : nullptr;
}

LoadedPlugin* PluginSystem::PluginAt(std::size_t index) noexcept {
    return index < plugins_.size() ? &plugins_[index] : nullptr;
}

std::size_t PluginSystem::EnabledCount() const noexcept {
    return static_cast<std::size_t>(std::count_if(plugins_.begin(), plugins_.end(),
        [](const LoadedPlugin& plugin) { return plugin.valid && plugin.enabled; }));
}

std::size_t PluginSystem::ErrorCount() const noexcept {
    return static_cast<std::size_t>(std::count_if(plugins_.begin(), plugins_.end(),
        [](const LoadedPlugin& plugin) { return !plugin.valid; }));
}

std::size_t PluginSystem::ReviewCount() const noexcept {
    return static_cast<std::size_t>(std::count_if(plugins_.begin(), plugins_.end(),
        [](const LoadedPlugin& plugin) { return plugin.valid && plugin.permissionsChanged; }));
}

std::size_t PluginSystem::CommandCount() const noexcept {
    std::size_t count = 0;
    for (const auto& plugin : plugins_) if (plugin.valid && plugin.enabled) count += plugin.definition.commands.size();
    return count;
}

bool PluginSystem::SetEnabled(std::size_t index, bool enabled) {
    LoadedPlugin* plugin = PluginAt(index);
    if (!plugin || !plugin->valid) return false;
    if (enabled) {
        WritePermissionFingerprint(plugin->definition.id, PermissionFingerprint(plugin->definition));
        plugin->permissionsChanged = false;
    }
    plugin->enabled = enabled;
    WriteEnabledState(plugin->definition.id, enabled);
    if (!enabled && SamePluginId(activeThemeId_, plugin->definition.id)) ResetTheme();
    if (!enabled && SamePluginId(activeLocaleId_, plugin->definition.id)) ResetLocale();
    if (enabled) {
        // Re-enabling starts every timer's countdown fresh instead of firing
        // immediately off a stale schedule from before it was disabled.
        const std::wstring prefix = plugin->definition.id + L"#";
        for (auto it = timerNextFireMs_.begin(); it != timerNextFireMs_.end();) {
            if (it->first.rfind(prefix, 0) == 0) it = timerNextFireMs_.erase(it);
            else ++it;
        }
    }
    lastReloadSummary_ = std::to_wstring(plugins_.size()) + L" plugin(s) · " +
                         std::to_wstring(EnabledCount()) + L" enabled · " +
                         std::to_wstring(ReviewCount()) + L" review · " +
                         std::to_wstring(ErrorCount()) + L" error(s)";
    return true;
}

bool PluginSystem::ActivateTheme(std::size_t index) {
    LoadedPlugin* plugin = PluginAt(index);
    if (!plugin || !plugin->valid || !plugin->enabled || !plugin->definition.theme.present ||
        !LunaScript::HasPermission(plugin->definition, LunaPermission::Theme)) return false;
    activeThemeId_ = plugin->definition.id;
    WriteActiveTheme();
    return true;
}

void PluginSystem::ResetTheme() {
    activeThemeId_.clear();
    WriteActiveTheme();
}

const LoadedPlugin* PluginSystem::ActiveThemePlugin() const noexcept {
    if (activeThemeId_.empty()) return nullptr;
    for (const auto& plugin : plugins_) {
        if (plugin.valid && plugin.enabled && SamePluginId(plugin.definition.id, activeThemeId_) &&
            plugin.definition.theme.present) return &plugin;
    }
    return nullptr;
}

const LunaThemeDefinition* PluginSystem::ActiveTheme() const noexcept {
    const LoadedPlugin* plugin = ActiveThemePlugin();
    return plugin ? &plugin->definition.theme : nullptr;
}

bool PluginSystem::ActivateLocale(std::size_t index) {
    LoadedPlugin* plugin = PluginAt(index);
    if (!plugin || !plugin->valid || !plugin->enabled || !plugin->definition.locale.present ||
        !LunaScript::HasPermission(plugin->definition, LunaPermission::Localization)) return false;
    activeLocaleId_ = plugin->definition.id;
    WriteActiveLocale();
    return true;
}

void PluginSystem::ResetLocale() {
    activeLocaleId_.clear();
    WriteActiveLocale();
}

const LoadedPlugin* PluginSystem::ActiveLocalePlugin() const noexcept {
    if (activeLocaleId_.empty()) return nullptr;
    for (const auto& plugin : plugins_) {
        if (plugin.valid && plugin.enabled && SamePluginId(plugin.definition.id, activeLocaleId_) &&
            plugin.definition.locale.present) return &plugin;
    }
    return nullptr;
}

const LunaLocaleDefinition* PluginSystem::ActiveLocale() const noexcept {
    const LoadedPlugin* plugin = ActiveLocalePlugin();
    return plugin ? &plugin->definition.locale : nullptr;
}

PluginExecution PluginSystem::ExecuteCommand(std::size_t pluginIndex, std::size_t commandIndex) const {
    PluginExecution result;
    const LoadedPlugin* plugin = PluginAt(pluginIndex);
    if (!plugin || !plugin->valid || !plugin->enabled || commandIndex >= plugin->definition.commands.size()) return result;
    result.pluginId = plugin->definition.id;
    result.pluginName = plugin->definition.name;
    result.instructions = plugin->definition.commands[commandIndex].instructions;
    return result;
}

PluginExecution PluginSystem::ExecuteCommand(const std::wstring& pluginId, const std::wstring& commandId) const {
    PluginExecution result;
    for (const auto& plugin : plugins_) {
        if (!plugin.valid || !plugin.enabled || !SamePluginId(plugin.definition.id, pluginId)) continue;
        const auto command = std::find_if(plugin.definition.commands.begin(), plugin.definition.commands.end(),
            [&commandId](const LunaCommandDefinition& item) { return item.id == commandId; });
        if (command == plugin.definition.commands.end()) return result;
        result.pluginId = plugin.definition.id;
        result.pluginName = plugin.definition.name;
        result.instructions = command->instructions;
        return result;
    }
    return result;
}

std::vector<PluginExecution> PluginSystem::DispatchEvent(const std::wstring& eventName) {
    std::vector<PluginExecution> executions;

    struct EventCandidate {
        const LoadedPlugin* plugin = nullptr;
        const LunaEventDefinition* event = nullptr;
    };
    std::vector<EventCandidate> candidates;
    for (const auto& plugin : plugins_) {
        if (!plugin.valid || !plugin.enabled) continue;
        for (const auto& event : plugin.definition.events) {
            if (event.eventName != eventName || event.instructions.empty()) continue;
            candidates.push_back({&plugin, &event});
        }
    }

    std::vector<std::size_t> handlerSizes;
    handlerSizes.reserve(candidates.size());
    for (const auto& candidate : candidates) handlerSizes.push_back(candidate.event->instructions.size());
    const auto plan = eventDispatchScheduler_.Select(eventName, handlerSizes, kDispatchInstructionBudget);
    if (!plan) return executions;

    executions.reserve(plan->handlerIndices.size());
    for (const std::size_t index : plan->handlerIndices) {
        const auto& candidate = candidates[index];
        PluginExecution execution;
        execution.pluginId = candidate.plugin->definition.id;
        execution.pluginName = candidate.plugin->definition.name;
        execution.instructions = candidate.event->instructions;
        executions.push_back(std::move(execution));
    }
    return executions;
}

std::vector<PluginExecution> PluginSystem::DispatchDueTimers(std::uint64_t nowMs) {
    std::vector<PluginExecution> executions;
    std::size_t instructionBudget = kDispatchInstructionBudget;
    struct DueCandidate {
        const LoadedPlugin* plugin = nullptr;
        const LunaEventDefinition* event = nullptr;
        std::size_t eventIndex = 0;
    };
    std::vector<DueCandidate> candidates;
    for (const auto& plugin : plugins_) {
        if (!plugin.valid || !plugin.enabled) continue;
        for (std::size_t eventIndex = 0; eventIndex < plugin.definition.events.size(); ++eventIndex) {
            const auto& event = plugin.definition.events[eventIndex];
            if (event.eventName == L"timer" && event.intervalSeconds > 0 && !event.instructions.empty()) {
                candidates.push_back({&plugin, &event, eventIndex});
            }
        }
    }
    if (candidates.empty()) {
        timerDispatchCursor_ = 0;
        return executions;
    }

    const std::size_t start = timerDispatchCursor_ % candidates.size();
    for (std::size_t offset = 0; offset < candidates.size(); ++offset) {
        const std::size_t index = (start + offset) % candidates.size();
        const auto& candidate = candidates[index];
        const auto& plugin = *candidate.plugin;
        const auto& event = *candidate.event;
        const std::uint64_t intervalMs = static_cast<std::uint64_t>(event.intervalSeconds) * 1000;
        const std::wstring key = plugin.definition.id + L"#" + std::to_wstring(candidate.eventIndex);
        const auto inserted = timerNextFireMs_.try_emplace(key, nowMs + intervalMs);
        std::uint64_t& nextFire = inserted.first->second;
        if (nowMs < nextFire) continue;
        // A whole handler is atomic. If this one does not fit, begin the next
        // host tick here so large groups of equal-period timers cannot starve
        // handlers that sort later by path/plugin id.
        if (event.instructions.size() > instructionBudget) {
            timerDispatchCursor_ = index;
            return executions;
        }
        nextFire = nowMs + intervalMs;
        PluginExecution execution;
        execution.pluginId = plugin.definition.id;
        execution.pluginName = plugin.definition.name;
        execution.instructions = event.instructions;
        instructionBudget -= event.instructions.size();
        executions.push_back(std::move(execution));
    }
    timerDispatchCursor_ = (start + 1) % candidates.size();
    return executions;
}

std::filesystem::path PluginSystem::PluginDirectory(std::size_t index) const {
    const LoadedPlugin* plugin = PluginAt(index);
    return plugin ? plugin->sourcePath.parent_path() : rootDirectory_;
}

std::wstring PluginSystem::ReadUtf8File(const std::filesystem::path& path, bool& ok) const {
    ok = false;
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > 1024 * 1024) return {};
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF) {
        bytes.erase(0, 3);
    }
    return Utf8ToWide(bytes, ok);
}

std::wstring PluginSystem::ComputeDirectoryStamp() const {
    std::vector<std::filesystem::path> sources;
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator iterator(rootDirectory_,
             std::filesystem::directory_options::skip_permission_denied, error), end;
         !error && iterator != end; iterator.increment(error)) {
        if (!iterator->is_regular_file(error) || !IsLunaSource(iterator->path())) continue;
        sources.push_back(iterator->path());
    }
    std::sort(sources.begin(), sources.end());
    if (sources.size() > kMaximumPlugins) sources.resize(kMaximumPlugins);
    std::wostringstream stamp;
    for (const auto& source : sources) {
        stamp << source.wstring() << L'|' << std::filesystem::file_size(source, error) << L'|';
        const auto time = std::filesystem::last_write_time(source, error).time_since_epoch().count();
        stamp << time << L';';
        error.clear();
    }
    // Theme art is part of the parsed plugin package even though it is not a
    // .luna source. Include only canonical, plugin-local image paths so replacing,
    // adding, or removing an active/available theme asset triggers hot reload
    // without letting a plugin probe metadata outside its own directory.
    for (const auto& plugin : plugins_) {
        const auto asset = ThemeBackgroundAsset(plugin);
        if (!asset) continue;
        error.clear();
        const bool regular = std::filesystem::is_regular_file(*asset, error) && !error;
        stamp << L"asset|" << plugin.definition.id << L'|' << asset->wstring() << L'|'
              << (regular ? 1 : 0) << L'|';
        if (regular) {
            const auto size = std::filesystem::file_size(*asset, error);
            if (!error) stamp << size;
            stamp << L'|';
            error.clear();
            const auto time = std::filesystem::last_write_time(*asset, error).time_since_epoch().count();
            if (!error) stamp << time;
        }
        stamp << L';';
        error.clear();
    }
    return stamp.str();
}

bool PluginSystem::ReadEnabledState(const std::wstring& pluginId) const {
    const std::wstring key = CanonicalPluginId(pluginId);
    return GetPrivateProfileIntW(L"Plugins", key.c_str(), 0, statePath_.c_str()) != 0;
}

void PluginSystem::WriteEnabledState(const std::wstring& pluginId, bool enabled) const {
    const std::wstring key = CanonicalPluginId(pluginId);
    WritePrivateProfileStringW(L"Plugins", key.c_str(), enabled ? L"1" : L"0", statePath_.c_str());
}

std::wstring PluginSystem::ReadPermissionFingerprint(const std::wstring& pluginId) const {
    wchar_t buffer[1024]{};
    const std::wstring key = CanonicalPluginId(pluginId);
    GetPrivateProfileStringW(L"PermissionApprovals", key.c_str(), L"", buffer,
                             static_cast<DWORD>(std::size(buffer)), statePath_.c_str());
    return buffer;
}

void PluginSystem::WritePermissionFingerprint(const std::wstring& pluginId,
                                               const std::wstring& fingerprint) const {
    const std::wstring key = CanonicalPluginId(pluginId);
    WritePrivateProfileStringW(L"PermissionApprovals", key.c_str(), fingerprint.c_str(), statePath_.c_str());
}

void PluginSystem::ReadHostState() {
    wchar_t buffer[256]{};
    GetPrivateProfileStringW(L"Host", L"ActiveTheme", L"", buffer, static_cast<DWORD>(std::size(buffer)), statePath_.c_str());
    activeThemeId_ = buffer;
    GetPrivateProfileStringW(L"Host", L"ActiveLocale", L"", buffer, static_cast<DWORD>(std::size(buffer)), statePath_.c_str());
    activeLocaleId_ = buffer;
}

void PluginSystem::WriteActiveTheme() const {
    WritePrivateProfileStringW(L"Host", L"ActiveTheme", activeThemeId_.c_str(), statePath_.c_str());
}

void PluginSystem::WriteActiveLocale() const {
    WritePrivateProfileStringW(L"Host", L"ActiveLocale", activeLocaleId_.c_str(), statePath_.c_str());
}

void PluginSystem::LocateDocumentation() {
    wchar_t modulePath[32768]{};
    const DWORD count = GetModuleFileNameW(nullptr, modulePath, static_cast<DWORD>(std::size(modulePath)));
    if (count > 0 && count < std::size(modulePath)) {
        const auto candidate = std::filesystem::path(modulePath).parent_path() / L"docs" / L"plugin-sdk.html";
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error)) {
            documentationPath_ = candidate;
            return;
        }
    }
    documentationPath_ = std::filesystem::current_path() / L"docs" / L"plugin-sdk.html";
}
