#include "Installer.hpp"

#include <ShlObj.h>
#include <ShObjIdl.h>
#include <d3d11.h>
// security.h refuses to compile without one of these; we only want GetUserNameExW.
#define SECURITY_WIN32
#include <security.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <taskschd.h>
#include <winhttp.h>
#include <wrl/client.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "taskschd.lib")
#pragma comment(lib, "secur32.lib")

using Microsoft::WRL::ComPtr;

namespace installer {
namespace {

std::wstring ExecutableDirectory() {
    std::wstring path(MAX_PATH * 4, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(length);
    return std::filesystem::path(path).parent_path().wstring();
}

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &raw)) || !raw) {
        if (raw) CoTaskMemFree(raw);
        return {};
    }
    std::wstring value(raw);
    CoTaskMemFree(raw);
    return value;
}

std::wstring Trim(std::wstring value) {
    while (!value.empty() && (value.front() == L' ' || value.front() == L'\t' ||
                              value.front() == L'\r' || value.front() == L'\n')) {
        value.erase(value.begin());
    }
    while (!value.empty() && (value.back() == L' ' || value.back() == L'\t' ||
                              value.back() == L'\r' || value.back() == L'\n')) {
        value.pop_back();
    }
    return value;
}

std::vector<int> VersionParts(const std::wstring& text) {
    std::vector<int> parts;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t dot = text.find(L'.', start);
        const std::wstring token = text.substr(start, dot == std::wstring::npos ? std::wstring::npos : dot - start);
        try {
            parts.push_back(std::stoi(token));
        } catch (...) {
            parts.push_back(0);
        }
        if (dot == std::wstring::npos) break;
        start = dot + 1;
    }
    return parts;
}

bool IsNewer(const std::wstring& candidate, const std::wstring& reference) {
    const auto a = VersionParts(candidate);
    const auto b = VersionParts(reference);
    const std::size_t count = (std::max)(a.size(), b.size());
    for (std::size_t index = 0; index < count; ++index) {
        const int left = index < a.size() ? a[index] : 0;
        const int right = index < b.size() ? b[index] : 0;
        if (left != right) return left > right;
    }
    return false;
}

std::wstring Widen(const std::string& value) {
    if (value.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring result(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), needed);
    return result;
}

// --- HTTP -------------------------------------------------------------------

struct HttpResponse {
    std::string body;
    bool ok = false;
};

// Follows the host/path split for both api.github.com and the redirect target
// that release asset downloads land on.
bool SplitUrl(const std::wstring& url, std::wstring& host, std::wstring& path, INTERNET_PORT& port, bool& secure) {
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    wchar_t hostBuffer[256]{};
    wchar_t pathBuffer[2048]{};
    components.lpszHostName = hostBuffer;
    components.dwHostNameLength = static_cast<DWORD>(std::size(hostBuffer) - 1);
    components.lpszUrlPath = pathBuffer;
    components.dwUrlPathLength = static_cast<DWORD>(std::size(pathBuffer) - 1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &components)) return false;
    host.assign(hostBuffer);
    path.assign(pathBuffer);
    port = components.nPort;
    secure = components.nScheme == INTERNET_SCHEME_HTTPS;
    return true;
}

HttpResponse HttpGet(const std::wstring& url, const wchar_t* extraHeaders,
                     const ProgressCallback* progress, const std::wstring& progressMessage,
                     std::filesystem::path* saveTo) {
    HttpResponse response;
    std::wstring host, path;
    INTERNET_PORT port = 0;
    bool secure = true;
    if (!SplitUrl(url, host, path, port, secure)) return response;

    // GitHub rejects requests without a User-Agent outright.
    const HINTERNET session = WinHttpOpen(L"LunaCapture-Installer/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return response;
    WinHttpSetTimeouts(session, 8000, 8000, 20000, 20000);
    const HINTERNET connect = WinHttpConnect(session, host.c_str(), port, 0);
    if (!connect) { WinHttpCloseHandle(session); return response; }

    const HINTERNET request = WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                 WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
    if (request) {
        if (extraHeaders) WinHttpAddRequestHeaders(request, extraHeaders, static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD);
        if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(request, nullptr)) {
            DWORD status = 0;
            DWORD statusSize = sizeof(status);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
            if (status == 200) {
                std::uint64_t total = 0;
                DWORD lengthSize = sizeof(total);
                DWORD contentLength = 0;
                lengthSize = sizeof(contentLength);
                if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                        WINHTTP_HEADER_NAME_BY_INDEX, &contentLength, &lengthSize,
                                        WINHTTP_NO_HEADER_INDEX)) {
                    total = contentLength;
                }

                std::ofstream file;
                if (saveTo) file.open(*saveTo, std::ios::binary | std::ios::trunc);
                std::uint64_t received = 0;
                DWORD available = 0;
                bool failed = false;
                while (WinHttpQueryDataAvailable(request, &available) && available > 0) {
                    std::string chunk(available, '\0');
                    DWORD read = 0;
                    if (!WinHttpReadData(request, chunk.data(), available, &read)) { failed = true; break; }
                    chunk.resize(read);
                    received += read;
                    if (saveTo) file.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
                    else response.body += chunk;
                    if (progress && total > 0) {
                        const int percent = static_cast<int>((received * 100) / total);
                        (*progress)((std::min)(99, percent), progressMessage);
                    }
                    // A source archive can be large; cap in-memory reads only.
                    if (!saveTo && response.body.size() > (4u << 20)) break;
                }
                if (saveTo) file.close();
                response.ok = !failed && (saveTo ? received > 0 : !response.body.empty());
            }
        }
        WinHttpCloseHandle(request);
    }
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return response;
}

// --- JSON-ish extraction ----------------------------------------------------
// The releases payload is walked with bounded regexes instead of a full JSON
// parser: the installer only needs tag names and browser_download_url values,
// and a dependency-free installer is worth more than a strict parse here.

struct ReleaseEntry {
    std::wstring tag;
    std::vector<std::pair<std::wstring, std::wstring>> assets;  // name, url
};

std::vector<ReleaseEntry> ParseReleases(const std::string& body) {
    std::vector<ReleaseEntry> releases;
    // Split on the top-level "tag_name" markers; each slice belongs to one release.
    // Custom raw-string delimiters: the patterns themselves contain )" sequences.
    static const std::regex tagPattern(R"JSON("tag_name"\s*:\s*"([^"]+)")JSON");
    static const std::regex namePattern(R"JSON("name"\s*:\s*"([^"]*)")JSON");
    static const std::regex urlPattern(R"JSON("browser_download_url"\s*:\s*"([^"]+)")JSON");

    std::vector<std::pair<std::size_t, std::wstring>> tags;
    for (auto it = std::sregex_iterator(body.begin(), body.end(), tagPattern); it != std::sregex_iterator(); ++it) {
        tags.emplace_back(static_cast<std::size_t>(it->position()), Widen((*it)[1].str()));
    }
    for (std::size_t index = 0; index < tags.size(); ++index) {
        const std::size_t start = tags[index].first;
        const std::size_t end = index + 1 < tags.size() ? tags[index + 1].first : body.size();
        ReleaseEntry entry;
        entry.tag = tags[index].second;
        const std::string slice = body.substr(start, end - start);

        // Pair each download URL with the *nearest preceding* "name". A release
        // object carries its own "name" right after "tag_name", well before the
        // assets array, so matching forward from the first "name" attributed the
        // release title to the asset and hid every real download.
        std::vector<std::pair<std::size_t, std::string>> names;
        for (auto it = std::sregex_iterator(slice.begin(), slice.end(), namePattern);
             it != std::sregex_iterator(); ++it) {
            names.emplace_back(static_cast<std::size_t>(it->position()), (*it)[1].str());
        }
        for (auto it = std::sregex_iterator(slice.begin(), slice.end(), urlPattern);
             it != std::sregex_iterator(); ++it) {
            const auto urlPosition = static_cast<std::size_t>(it->position());
            const std::string* assetName = nullptr;
            for (const auto& [position, value] : names) {
                if (position < urlPosition) assetName = &value;
                else break;
            }
            if (!assetName || assetName->empty()) continue;
            entry.assets.emplace_back(Widen(*assetName), Widen((*it)[1].str()));
        }
        releases.push_back(std::move(entry));
    }
    return releases;
}

std::wstring NormalizeTag(std::wstring tag) {
    if (!tag.empty() && (tag.front() == L'v' || tag.front() == L'V')) tag.erase(tag.begin());
    return tag;
}

bool PickAsset(const ReleaseEntry& entry, ReleaseInfo& release) {
    const auto lowered = [](std::wstring value) {
        std::transform(value.begin(), value.end(), value.begin(), ::towlower);
        return value;
    };
    const auto endsWith = [](const std::wstring& value, const std::wstring& suffix) {
        return value.size() > suffix.size() && value.rfind(suffix) == value.size() - suffix.size();
    };
    const auto accept = [&release](const std::wstring& name, const std::wstring& url, bool archive) {
        release.assetName = name;
        release.downloadUrl = url;
        release.isArchive = archive;
        return true;
    };

    const std::wstring wanted = lowered(kPortableArchiveName);

    // 1. The exact portable archive this installer is built around.
    for (const auto& [name, url] : entry.assets) {
        if (lowered(name) == wanted) return accept(name, url, true);
    }
    // 2. A versioned variant of the same thing, e.g. LunaCapture-portable-1.0.0.1.zip.
    for (const auto& [name, url] : entry.assets) {
        const std::wstring lower = lowered(name);
        if (endsWith(lower, L".zip") && lower.find(L"portable") != std::wstring::npos) {
            return accept(name, url, true);
        }
    }
    // 3. Any other build archive. GitHub's automatic source archives are useless
    //    to an installer, so they are explicitly excluded.
    for (const auto& [name, url] : entry.assets) {
        const std::wstring lower = lowered(name);
        if (endsWith(lower, L".zip") && lower.find(L"source") == std::wstring::npos) {
            return accept(name, url, true);
        }
    }
    // 4. Last resort: a bare executable attached without an archive.
    for (const auto& [name, url] : entry.assets) {
        if (endsWith(lowered(name), L".exe")) return accept(name, url, false);
    }
    return false;
}

// --- Shell helpers ----------------------------------------------------------

bool CreateShortcutFile(const std::wstring& target, const std::wstring& linkPath,
                        const std::wstring& arguments, const std::wstring& description) {
    ComPtr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) return false;
    link->SetPath(target.c_str());
    if (!arguments.empty()) link->SetArguments(arguments.c_str());
    link->SetDescription(description.c_str());
    link->SetWorkingDirectory(std::filesystem::path(target).parent_path().c_str());
    link->SetIconLocation(target.c_str(), 0);
    ComPtr<IPersistFile> persist;
    if (FAILED(link.As(&persist))) return false;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(linkPath).parent_path(), ec);
    return SUCCEEDED(persist->Save(linkPath.c_str(), TRUE));
}

bool WriteStringValue(HKEY root, const std::wstring& subKey, const wchar_t* name, const std::wstring& value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(root, subKey.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const LSTATUS status = RegSetValueExW(key, name, 0, REG_SZ,
                                          reinterpret_cast<const BYTE*>(value.c_str()),
                                          static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

std::wstring ReadStringValue(HKEY root, const std::wstring& subKey, const wchar_t* name) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subKey.c_str(), 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return {};
    wchar_t buffer[1024]{};
    DWORD size = sizeof(buffer) - sizeof(wchar_t);
    DWORD type = 0;
    std::wstring value;
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(buffer), &size) == ERROR_SUCCESS &&
        type == REG_SZ) {
        value.assign(buffer);
    }
    RegCloseKey(key);
    return value;
}

std::wstring UninstallKeyPath() {
    return std::wstring(L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\") + kUninstallKeyName;
}

void SetRunAsAdminFlag(const std::wstring& exePath, bool enable) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
                        L"Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers",
                        0, nullptr, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return;
    }
    if (enable) {
        const std::wstring value = L"~ RUNASADMIN";
        RegSetValueExW(key, exePath.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                       static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, exePath.c_str());
    }
    RegCloseKey(key);
}

std::wstring CurrentUserSamName() {
    ULONG size = 0;
    GetUserNameExW(NameSamCompatible, nullptr, &size);
    if (size == 0) return {};
    std::wstring name(size, L'\0');
    if (!GetUserNameExW(NameSamCompatible, name.data(), &size)) return {};
    name.resize(wcsnlen(name.c_str(), name.size()));
    return name;
}

std::wstring XmlEscape(const std::wstring& value) {
    std::wstring out;
    out.reserve(value.size());
    for (const wchar_t ch : value) {
        switch (ch) {
            case L'&': out += L"&amp;"; break;
            case L'<': out += L"&lt;"; break;
            case L'>': out += L"&gt;"; break;
            case L'"': out += L"&quot;"; break;
            case L'\'': out += L"&apos;"; break;
            default: out.push_back(ch); break;
        }
    }
    return out;
}

// "Start with Windows" and "always run as administrator" cannot both be served
// by a Startup-folder shortcut: Explorer's sign-in launcher never shows a UAC
// prompt, so a RUNASADMIN shim makes it silently skip the app. A logon-triggered
// scheduled task running with the highest available privileges is the supported
// way to get both, and it produces no prompt at sign-in.
bool RegisterElevatedLogonTask(const std::wstring& exePath, std::wstring& error) {
    const std::wstring user = CurrentUserSamName();
    if (user.empty()) {
        error = L"The current Windows account name could not be determined.";
        return false;
    }

    ComPtr<ITaskService> service;
    if (FAILED(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&service)))) {
        error = L"The Windows Task Scheduler service is unavailable.";
        return false;
    }
    VARIANT empty{};
    empty.vt = VT_EMPTY;
    if (FAILED(service->Connect(empty, empty, empty, empty))) {
        error = L"Could not connect to the Task Scheduler service.";
        return false;
    }
    ComPtr<ITaskFolder> folder;
    BSTR rootPath = SysAllocString(L"\\");
    const HRESULT folderResult = service->GetFolder(rootPath, &folder);
    SysFreeString(rootPath);
    if (FAILED(folderResult) || !folder) {
        error = L"The Task Scheduler root folder could not be opened.";
        return false;
    }

    const std::wstring escapedUser = XmlEscape(user);
    const std::wstring xml =
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">"
        L"<RegistrationInfo><Author>" + escapedUser + L"</Author>"
        L"<Description>Starts Luna Capture with administrator rights when you sign in.</Description>"
        L"</RegistrationInfo>"
        L"<Triggers><LogonTrigger><Enabled>true</Enabled><UserId>" + escapedUser + L"</UserId></LogonTrigger></Triggers>"
        L"<Principals><Principal id=\"Author\"><UserId>" + escapedUser + L"</UserId>"
        L"<LogonType>InteractiveToken</LogonType><RunLevel>HighestAvailable</RunLevel></Principal></Principals>"
        L"<Settings>"
        L"<MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>"
        L"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
        L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>"
        L"<AllowHardTerminate>false</AllowHardTerminate>"
        L"<StartWhenAvailable>false</StartWhenAvailable>"
        L"<RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>"
        L"<IdleSettings><StopOnIdleEnd>false</StopOnIdleEnd><RestartOnIdle>false</RestartOnIdle></IdleSettings>"
        L"<AllowStartOnDemand>true</AllowStartOnDemand><Enabled>true</Enabled><Hidden>false</Hidden>"
        L"<RunOnlyIfIdle>false</RunOnlyIfIdle><WakeToRun>false</WakeToRun>"
        L"<ExecutionTimeLimit>PT0S</ExecutionTimeLimit><Priority>7</Priority>"
        L"</Settings>"
        L"<Actions Context=\"Author\"><Exec><Command>" + XmlEscape(exePath) + L"</Command>"
        L"<Arguments>--background</Arguments>"
        L"<WorkingDirectory>" + XmlEscape(std::filesystem::path(exePath).parent_path().wstring()) +
        L"</WorkingDirectory></Exec></Actions>"
        L"</Task>";

    BSTR taskName = SysAllocString(kElevatedTaskName);
    BSTR taskXml = SysAllocString(xml.c_str());
    VARIANT nullVariant{};
    nullVariant.vt = VT_NULL;
    ComPtr<IRegisteredTask> registered;
    const HRESULT result = folder->RegisterTask(taskName, taskXml, TASK_CREATE_OR_UPDATE, nullVariant,
                                                nullVariant, TASK_LOGON_INTERACTIVE_TOKEN, nullVariant,
                                                &registered);
    SysFreeString(taskName);
    SysFreeString(taskXml);
    if (FAILED(result)) {
        error = result == E_ACCESSDENIED
            ? std::wstring(L"Creating the sign-in task needs administrator rights. Right-click the installer "
                           L"and choose \"Run as administrator\", or clear \"Always run as administrator\".")
            : L"The sign-in task could not be created (0x" + std::to_wstring(static_cast<unsigned long>(result)) + L").";
        return false;
    }
    return true;
}

void RemoveElevatedLogonTask() {
    ComPtr<ITaskService> service;
    if (FAILED(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&service)))) return;
    VARIANT empty{};
    empty.vt = VT_EMPTY;
    if (FAILED(service->Connect(empty, empty, empty, empty))) return;
    ComPtr<ITaskFolder> folder;
    BSTR rootPath = SysAllocString(L"\\");
    const HRESULT folderResult = service->GetFolder(rootPath, &folder);
    SysFreeString(rootPath);
    if (FAILED(folderResult) || !folder) return;
    BSTR taskName = SysAllocString(kElevatedTaskName);
    folder->DeleteTask(taskName, 0);
    SysFreeString(taskName);
}

// Expands a .zip using the Shell's own folder copy — no third-party archive
// dependency, and it is present on every supported Windows build.
bool ExpandArchive(const std::filesystem::path& archive, const std::filesystem::path& destination,
                   std::wstring& error) {
    std::error_code ec;
    std::filesystem::create_directories(destination, ec);

    ComPtr<IShellDispatch> shell;
    if (FAILED(CoCreateInstance(CLSID_Shell, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shell)))) {
        error = L"The Windows Shell automation service is unavailable, so the archive could not be expanded.";
        return false;
    }
    VARIANT source{};
    source.vt = VT_BSTR;
    source.bstrVal = SysAllocString(archive.c_str());
    VARIANT target{};
    target.vt = VT_BSTR;
    target.bstrVal = SysAllocString(destination.c_str());
    VARIANT flags{};
    flags.vt = VT_I4;
    flags.lVal = 16 | 4 | 512;  // Yes-to-all, no progress UI, no confirmation

    ComPtr<Folder> sourceFolder;
    ComPtr<Folder> targetFolder;
    bool ok = false;
    if (SUCCEEDED(shell->NameSpace(source, &sourceFolder)) && sourceFolder &&
        SUCCEEDED(shell->NameSpace(target, &targetFolder)) && targetFolder) {
        ComPtr<FolderItems> items;
        if (SUCCEEDED(sourceFolder->Items(&items)) && items) {
            VARIANT itemsVariant{};
            itemsVariant.vt = VT_DISPATCH;
            itemsVariant.pdispVal = items.Get();
            ok = SUCCEEDED(targetFolder->CopyHere(itemsVariant, flags));
        }
    }
    SysFreeString(source.bstrVal);
    SysFreeString(target.bstrVal);
    if (!ok) error = L"The downloaded archive could not be expanded.";
    return ok;
}

// The Shell copy above is asynchronous; wait until the executable shows up.
bool WaitForExtractedExecutable(const std::filesystem::path& directory, std::filesystem::path& found,
                                DWORD timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (GetTickCount64() < deadline) {
        std::error_code ec;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(directory, ec)) {
            if (ec) break;
            if (entry.is_regular_file(ec) && entry.path().filename() == kExecutableName) {
                found = entry.path();
                return true;
            }
        }
        Sleep(200);
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------

std::wstring ReadPinnedVersion() {
    const std::filesystem::path config = std::filesystem::path(ExecutableDirectory()) / L"installer.ini";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(config, ec)) return {};
    std::wifstream file(config);
    std::wstring line;
    while (std::getline(file, line)) {
        line = Trim(line);
        if (line.empty() || line.front() == L'#' || line.front() == L';') continue;
        const auto equals = line.find(L'=');
        if (equals == std::wstring::npos) continue;
        std::wstring key = Trim(line.substr(0, equals));
        std::wstring value = Trim(line.substr(equals + 1));
        std::transform(key.begin(), key.end(), key.begin(), ::towlower);
        if (key == L"targetversion" || key == L"version") {
            if (value == L"latest" || value == L"newest") return {};
            return value;
        }
    }
    return {};
}

std::wstring DefaultInstallDirectory() {
    const std::wstring base = KnownFolder(FOLDERID_LocalAppData);
    if (base.empty()) return L"C:\\Luna Capture";
    return (std::filesystem::path(base) / L"Programs" / L"Luna Capture").wstring();
}

std::wstring InstalledExecutablePath(const std::wstring& installDirectory) {
    return (std::filesystem::path(installDirectory) / kExecutableName).wstring();
}

DependencyReport CheckDependencies() {
    DependencyReport report;

    // Windows 10 1903 or newer. RtlGetVersion avoids the manifest-dependent
    // lie that GetVersionEx returns on unmanifested processes.
    OSVERSIONINFOEXW info{sizeof(info)};
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOEXW);
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
        if (auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(
                reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")))) {
            rtlGetVersion(&info);
        }
    }
    report.windowsVersionOk = info.dwMajorVersion > 10 ||
                              (info.dwMajorVersion == 10 && info.dwBuildNumber >= 18362);
    if (!report.windowsVersionOk) {
        report.windowsHint = L"Luna Capture needs Windows 10 version 1903 (build 18362) or newer.";
    }

    // Desktop Duplication needs a working D3D11 device.
    ComPtr<ID3D11Device> device;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
    D3D_FEATURE_LEVEL created{};
    report.direct3DOk = SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels,
                                                     static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
                                                     &device, &created, nullptr));
    if (!report.direct3DOk) {
        report.direct3DHint =
            L"No Direct3D 11 hardware device was found. Update your graphics driver — capture will not work "
            L"without one.";
    }
    return report;
}

bool ResolveRelease(const std::wstring& pinnedVersion, ReleaseInfo& release,
                    std::wstring& newerAvailable, std::wstring& error) {
    newerAvailable.clear();
    const std::wstring url = std::wstring(L"https://api.github.com/repos/") + kRepositoryOwner + L"/" +
                             kRepositoryName + L"/releases";
    const HttpResponse response = HttpGet(url, L"Accept: application/vnd.github+json\r\n", nullptr, {}, nullptr);
    if (!response.ok) {
        error = L"Could not reach GitHub. Check your internet connection, then press Retry.";
        return false;
    }
    const auto releases = ParseReleases(response.body);
    if (releases.empty()) {
        error = L"This project has no published releases yet, so there is nothing to install.";
        return false;
    }

    // Newest published version, used both as the default target and to offer an
    // upgrade when the pinned version is behind.
    std::wstring newestVersion;
    const ReleaseEntry* newestEntry = nullptr;
    for (const auto& entry : releases) {
        const std::wstring version = NormalizeTag(entry.tag);
        if (newestVersion.empty() || IsNewer(version, newestVersion)) {
            newestVersion = version;
            newestEntry = &entry;
        }
    }

    const ReleaseEntry* chosen = nullptr;
    std::wstring chosenVersion;
    if (pinnedVersion.empty()) {
        chosen = newestEntry;
        chosenVersion = newestVersion;
    } else {
        for (const auto& entry : releases) {
            if (NormalizeTag(entry.tag) == pinnedVersion) {
                chosen = &entry;
                chosenVersion = pinnedVersion;
                break;
            }
        }
        if (!chosen) {
            error = L"Release " + pinnedVersion + L" was not found on GitHub. Edit installer.ini to name an "
                    L"existing version, or set TargetVersion=latest.";
            return false;
        }
        if (!newestVersion.empty() && IsNewer(newestVersion, pinnedVersion)) newerAvailable = newestVersion;
    }

    release.version = chosenVersion;
    if (!PickAsset(*chosen, release)) {
        error = L"Release " + chosenVersion + L" has no " + kPortableArchiveName +
                L" attached — only GitHub's automatic source archives. Build it with build_release.bat, "
                L"attach it to the release, then press Retry.";
        return false;
    }
    return true;
}

bool RunInstall(const InstallOptions& options, const ReleaseInfo& release,
                const ProgressCallback& progress, std::wstring& error, std::wstring& warning) {
    warning.clear();
    const auto report = [&](int percent, const std::wstring& message) {
        if (progress) progress(percent, message);
    };

    std::error_code ec;
    const std::filesystem::path installDir(options.installDirectory);
    std::filesystem::create_directories(installDir, ec);
    if (ec) {
        error = L"The install folder could not be created. Pick a different location.";
        return false;
    }

    // Writability probe before spending a download on a folder we cannot use.
    {
        const std::filesystem::path probe = installDir / L".lunacapture-write-test";
        std::ofstream test(probe, std::ios::binary);
        if (!test) {
            error = L"That folder is not writable by your account. Pick a different location.";
            return false;
        }
        test.close();
        std::filesystem::remove(probe, ec);
    }

    report(2, L"Preparing download…");
    wchar_t tempPath[MAX_PATH]{};
    GetTempPathW(static_cast<DWORD>(std::size(tempPath)), tempPath);
    const std::filesystem::path staging =
        std::filesystem::path(tempPath) / (L"LunaCaptureSetup-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(staging, ec);
    const std::filesystem::path downloadPath = staging / release.assetName;

    auto downloadProgress = ProgressCallback([&](int percent, const std::wstring& message) {
        report(5 + (percent * 65) / 100, message);
    });
    const HttpResponse download = HttpGet(release.downloadUrl, nullptr, &downloadProgress,
                                          L"Downloading " + release.assetName + L"…",
                                          const_cast<std::filesystem::path*>(&downloadPath));
    if (!download.ok) {
        error = L"The download did not complete. Check your connection and try again.";
        std::filesystem::remove_all(staging, ec);
        return false;
    }

    // Upgrading over a running copy would fail on the locked executable, so ask
    // it to exit first. The app is single-instance and its window class is fixed.
    if (HWND running = FindWindowW(L"LunaCaptureMainWindow", nullptr)) {
        report(70, L"Closing the running copy…");
        PostMessageW(running, WM_CLOSE, 0, 0);
        for (int attempt = 0; attempt < 40 && IsWindow(running); ++attempt) Sleep(100);
    }

    report(72, L"Placing files…");
    const std::filesystem::path targetExe = installDir / kExecutableName;
    if (release.isArchive) {
        const std::filesystem::path expanded = staging / L"expanded";
        if (!ExpandArchive(downloadPath, expanded, error)) {
            std::filesystem::remove_all(staging, ec);
            return false;
        }
        std::filesystem::path extractedExe;
        if (!WaitForExtractedExecutable(expanded, extractedExe, 60000)) {
            error = L"The archive did not contain " + std::wstring(kExecutableName) + L".";
            std::filesystem::remove_all(staging, ec);
            return false;
        }
        // Copy the whole payload folder so bundled docs/assets come along.
        const std::filesystem::path payloadRoot = extractedExe.parent_path();
        for (const auto& entry : std::filesystem::recursive_directory_iterator(payloadRoot, ec)) {
            if (ec) break;
            const auto relative = std::filesystem::relative(entry.path(), payloadRoot, ec);
            if (ec || relative.empty()) continue;
            const auto destination = installDir / relative;
            if (entry.is_directory(ec)) {
                std::filesystem::create_directories(destination, ec);
            } else if (entry.is_regular_file(ec)) {
                std::filesystem::create_directories(destination.parent_path(), ec);
                std::filesystem::copy_file(entry.path(), destination,
                                           std::filesystem::copy_options::overwrite_existing, ec);
            }
        }
    } else {
        std::filesystem::copy_file(downloadPath, targetExe,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            error = L"The executable could not be written. Close Luna Capture if it is running and try again.";
            std::filesystem::remove_all(staging, ec);
            return false;
        }
    }
    std::filesystem::remove_all(staging, ec);

    if (!std::filesystem::is_regular_file(targetExe, ec)) {
        error = L"The install finished without producing " + std::wstring(kExecutableName) + L".";
        return false;
    }

    const std::wstring exePath = targetExe.wstring();

    report(80, L"Creating shortcuts…");
    if (options.startMenuShortcut) {
        const std::wstring programs = KnownFolder(FOLDERID_Programs);
        if (!programs.empty()) {
            CreateShortcutFile(exePath, (std::filesystem::path(programs) / L"Luna Capture.lnk").wstring(), {},
                               L"Luna Capture");
        }
    }
    if (options.desktopShortcut) {
        const std::wstring desktop = KnownFolder(FOLDERID_Desktop);
        if (!desktop.empty()) {
            CreateShortcutFile(exePath, (std::filesystem::path(desktop) / L"Luna Capture.lnk").wstring(), {},
                               L"Luna Capture");
        }
    }
    // Autostart has two implementations. A plain Startup shortcut is enough on its
    // own, but combined with "always run as administrator" it silently never
    // launches, so that pairing goes through a logon task instead.
    std::wstring autostartWarning;
    const std::wstring startup = KnownFolder(FOLDERID_Startup);
    const std::filesystem::path startupShortcut =
        startup.empty() ? std::filesystem::path{} : std::filesystem::path(startup) / L"LunaCapture.lnk";
    RemoveElevatedLogonTask();
    if (!startupShortcut.empty()) std::filesystem::remove(startupShortcut, ec);
    ec.clear();

    if (options.startWithWindows && options.runAsAdmin) {
        report(84, L"Creating the elevated sign-in task…");
        if (!RegisterElevatedLogonTask(exePath, autostartWarning)) {
            // Fall back to a plain shortcut so autostart still works, minus the
            // elevation, and say so on the final page rather than failing silently.
            if (!startupShortcut.empty()) {
                CreateShortcutFile(exePath, startupShortcut.wstring(), L"--background", L"Luna Capture");
            }
        } else {
            autostartWarning.clear();
        }
    } else if (options.startWithWindows && !startupShortcut.empty()) {
        CreateShortcutFile(exePath, startupShortcut.wstring(), L"--background", L"Luna Capture");
    }

    report(88, L"Applying options…");
    SetRunAsAdminFlag(exePath, options.runAsAdmin);

    report(95, L"Registering uninstaller…");
    {
        // The installer copies itself next to the app so uninstall keeps working
        // after the original download is deleted.
        std::wstring selfPath(MAX_PATH * 4, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, selfPath.data(), static_cast<DWORD>(selfPath.size()));
        selfPath.resize(length);
        const std::filesystem::path uninstaller = installDir / L"LunaUninstall.exe";
        std::filesystem::copy_file(selfPath, uninstaller, std::filesystem::copy_options::overwrite_existing, ec);

        const std::wstring key = UninstallKeyPath();
        WriteStringValue(HKEY_CURRENT_USER, key, L"DisplayName", kProductName);
        WriteStringValue(HKEY_CURRENT_USER, key, L"DisplayVersion", release.version);
        WriteStringValue(HKEY_CURRENT_USER, key, L"Publisher", L"ggrolton123");
        WriteStringValue(HKEY_CURRENT_USER, key, L"InstallLocation", options.installDirectory);
        WriteStringValue(HKEY_CURRENT_USER, key, L"DisplayIcon", exePath);
        WriteStringValue(HKEY_CURRENT_USER, key, L"URLInfoAbout",
                         std::wstring(L"https://github.com/") + kRepositoryOwner + L"/" + kRepositoryName);
        if (!ec) {
            WriteStringValue(HKEY_CURRENT_USER, key, L"UninstallString",
                             L"\"" + uninstaller.wstring() + L"\" --uninstall");
        }
        HKEY handle = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &handle,
                            nullptr) == ERROR_SUCCESS) {
            const DWORD noModify = 1;
            RegSetValueExW(handle, L"NoModify", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&noModify),
                           sizeof(noModify));
            RegSetValueExW(handle, L"NoRepair", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&noModify),
                           sizeof(noModify));
            RegCloseKey(handle);
        }
    }

    warning = autostartWarning;
    report(100, L"Installation complete.");
    return true;
}

std::wstring DetectExistingInstall() {
    return ReadStringValue(HKEY_CURRENT_USER, UninstallKeyPath(), L"InstallLocation");
}

std::wstring DetectInstalledVersion() {
    return ReadStringValue(HKEY_CURRENT_USER, UninstallKeyPath(), L"DisplayVersion");
}

bool RunUninstall(bool keepUserData, const ProgressCallback& progress, std::wstring& error) {
    const auto report = [&](int percent, const std::wstring& message) {
        if (progress) progress(percent, message);
    };

    const std::wstring installDirectory = DetectExistingInstall();
    if (installDirectory.empty()) {
        error = L"Luna Capture does not appear to be installed for this account.";
        return false;
    }
    const std::wstring exePath = InstalledExecutablePath(installDirectory);

    report(5, L"Closing Luna Capture…");
    // The app is single-instance; ask its window to quit before deleting files.
    if (HWND running = FindWindowW(L"LunaCaptureMainWindow", nullptr)) {
        PostMessageW(running, WM_CLOSE, 0, 0);
        Sleep(600);
    }

    report(20, L"Removing shortcuts…");
    RemoveElevatedLogonTask();
    std::error_code ec;
    for (const auto& [folder, name] : std::initializer_list<std::pair<KNOWNFOLDERID, const wchar_t*>>{
             {FOLDERID_Desktop, L"Luna Capture.lnk"},
             {FOLDERID_Programs, L"Luna Capture.lnk"},
             {FOLDERID_Startup, L"LunaCapture.lnk"}}) {
        const std::wstring base = KnownFolder(folder);
        if (base.empty()) continue;
        std::filesystem::remove(std::filesystem::path(base) / name, ec);
    }
    {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER,
                          L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\StartupFolder",
                          0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
            RegDeleteValueW(key, L"LunaCapture.lnk");
            RegCloseKey(key);
        }
    }

    SetRunAsAdminFlag(exePath, false);

    report(60, L"Deleting program files…");
    // Everything except the running uninstaller itself.
    std::wstring selfPath(MAX_PATH * 4, L'\0');
    const DWORD selfLength = GetModuleFileNameW(nullptr, selfPath.data(), static_cast<DWORD>(selfPath.size()));
    selfPath.resize(selfLength);
    const std::filesystem::path self(selfPath);
    for (const auto& entry : std::filesystem::directory_iterator(installDirectory, ec)) {
        if (ec) break;
        if (entry.path() == self) continue;
        std::filesystem::remove_all(entry.path(), ec);
        ec.clear();
    }

    if (!keepUserData) {
        report(80, L"Removing settings…");
        const std::wstring localAppData = KnownFolder(FOLDERID_LocalAppData);
        if (!localAppData.empty()) {
            std::filesystem::remove_all(std::filesystem::path(localAppData) / L"Luna Capture", ec);
            ec.clear();
        }
    }

    report(92, L"Removing the uninstall entry…");
    RegDeleteTreeW(HKEY_CURRENT_USER, UninstallKeyPath().c_str());

    // The uninstaller cannot delete itself while running; hand that to cmd.
    const std::wstring command = L"/c timeout /t 2 /nobreak >nul & del /f /q \"" + self.wstring() +
                                 L"\" & rmdir \"" + installDirectory + L"\"";
    SHELLEXECUTEINFOW execute{sizeof(execute)};
    execute.fMask = SEE_MASK_NOCLOSEPROCESS;
    execute.lpVerb = L"open";
    execute.lpFile = L"cmd.exe";
    execute.lpParameters = command.c_str();
    execute.nShow = SW_HIDE;
    ShellExecuteExW(&execute);
    if (execute.hProcess) CloseHandle(execute.hProcess);

    report(100, L"Luna Capture has been removed.");
    return true;
}

}  // namespace installer
