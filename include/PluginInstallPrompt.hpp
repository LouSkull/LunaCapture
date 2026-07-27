#pragma once

#include <filesystem>
#include <string>

// Result of showing the standalone plugin installer for a .luna file.
struct PluginInstallOutcome {
    bool installed = false;
    bool cancelled = false;
    std::wstring fileName;
    std::wstring pluginName;
    std::wstring failureReason;  // empty when the user simply cancelled
};

// Shows a small self-contained installer window for `sourcePath`: plugin name,
// author, description and the exact permission set it asks for, then copies it
// into the plugin folder on confirmation.
//
// It never depends on a running Luna Capture. Handing the file to an existing
// instance is unreliable — the window may be hidden in the tray, and an elevated
// instance cannot receive messages from a normal-integrity process at all — so
// this writes the plugin folder directly and lets the app's hot reload notice.
PluginInstallOutcome ShowPluginInstallPrompt(const std::wstring& sourcePath);
