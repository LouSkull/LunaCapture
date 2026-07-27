#include "UpdateCheck.hpp"

#include <winhttp.h>

#include <regex>
#include <thread>
#include <vector>

namespace {

std::vector<int> ParseVersionParts(const std::wstring& text) {
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

bool IsNewerVersion(const std::wstring& candidate, const std::wstring& current) {
    const auto a = ParseVersionParts(candidate);
    const auto b = ParseVersionParts(current);
    const std::size_t count = (std::max)(a.size(), b.size());
    for (std::size_t i = 0; i < count; ++i) {
        const int av = i < a.size() ? a[i] : 0;
        const int bv = i < b.size() ? b[i] : 0;
        if (av != bv) return av > bv;
    }
    return false;
}

// GitHub's releases API returns JSON whose release tags live in "tag_name" fields, tagged with or
// without a leading "v" (LouSkull/LunaCapture has both "2.0.0.0" and "v1.0.0.0"). Rather than a
// strict JSON parse, collect every three- or four-part-dotted version token in the response and
// keep the numerically highest one - robust to tag prefixes, extra JSON fields, and pre-releases.
bool ExtractVersionTag(const std::string& body, std::wstring& outVersion) {
    static const std::regex versionPattern(R"(v?(\d{1,4}\.\d{1,4}\.\d{1,4}(?:\.\d{1,4})?))");
    bool found = false;
    std::wstring best = L"0.0.0.0";
    for (auto it = std::sregex_iterator(body.begin(), body.end(), versionPattern); it != std::sregex_iterator(); ++it) {
        const std::string tag = (*it)[1].str();
        const std::wstring candidate(tag.begin(), tag.end());
        if (IsNewerVersion(candidate, best)) {
            best = candidate;
            found = true;
        }
    }
    if (!found) return false;
    outVersion = best;
    return true;
}

bool FetchResponseBody(std::string& outBody) {
    // GitHub requires a non-empty User-Agent or it rejects the request outright.
    const HINTERNET session = WinHttpOpen(L"LunaCapture-UpdateCheck/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    WinHttpSetTimeouts(session, 5000, 5000, 5000, 5000);

    const HINTERNET connect = WinHttpConnect(session, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connect) { WinHttpCloseHandle(session); return false; }

    const HINTERNET request = WinHttpOpenRequest(connect, L"GET",
                                                 L"/repos/LouSkull/LunaCapture/releases",
                                                 nullptr, WINHTTP_NO_REFERER,
                                                 WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    bool ok = false;
    if (request) {
        const wchar_t* headers = L"Accept: application/vnd.github+json\r\n";
        if (WinHttpSendRequest(request, headers, static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(request, nullptr)) {
            DWORD status = 0;
            DWORD statusSize = sizeof(status);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
            if (status == 200) {
                constexpr std::size_t kMaxBody = 1u << 20;  // 1 MiB caps a large releases list.
                DWORD available = 0;
                while (WinHttpQueryDataAvailable(request, &available) && available > 0) {
                    std::string chunk(available, '\0');
                    DWORD read = 0;
                    if (!WinHttpReadData(request, chunk.data(), available, &read)) break;
                    chunk.resize(read);
                    outBody += chunk;
                    if (outBody.size() > kMaxBody) break;
                }
                ok = !outBody.empty();
            }
        }
        WinHttpCloseHandle(request);
    }
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return ok;
}

}  // namespace

void StartUpdateCheck(HWND notifyWindow, UINT message, bool manual) {
    std::thread([notifyWindow, message, manual]() {
        std::string body;
        std::wstring latest;
        auto* result = new UpdateCheckResult();
        result->manual = manual;
        if (!FetchResponseBody(body) || !ExtractVersionTag(body, latest)) {
            result->outcome = UpdateCheckOutcome::Failed;
        } else {
            result->latestVersion = latest;
            result->outcome = IsNewerVersion(latest, kAppVersion) ? UpdateCheckOutcome::UpdateAvailable
                                                                  : UpdateCheckOutcome::UpToDate;
        }
        if (!PostMessageW(notifyWindow, message, 0, reinterpret_cast<LPARAM>(result))) delete result;
    }).detach();
}
