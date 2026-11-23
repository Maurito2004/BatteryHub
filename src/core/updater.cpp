#include "updater.h"

#include <windows.h>
#include <winhttp.h>

#include <cwctype>
#include <string>
#include <vector>

#include "../../assets/resource.h"
#include "device.h"
#include "log.h"

#ifdef _MSC_VER
#pragma comment(lib, "winhttp.lib")
#endif

namespace bh {
namespace {

std::wstring ExePath() {
    wchar_t p[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, p, MAX_PATH);
    return std::wstring(p, n);
}

// GET over HTTPS following redirects (GitHub assets redirect to a CDN).
bool HttpGet(const std::wstring& url, std::string* body, std::wstring* error, size_t maxBytes) {
    URL_COMPONENTS uc = {sizeof(uc)};
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = ARRAYSIZE(host);
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = ARRAYSIZE(path);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
        *error = L"URL inválida";
        return false;
    }
    std::wstring agent = L"BatteryHub/" + CurrentVersion();
    HINTERNET s = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s)  // WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY needs Windows 8.1+
        s = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) {
        *error = Fmt(L"WinHttpOpen (%lu)", GetLastError());
        return false;
    }
    WinHttpSetTimeouts(s, 10000, 10000, 15000, 30000);
    bool ok = false;
    HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                    : nullptr;
    if (r && WinHttpSendRequest(r, L"Accept: application/vnd.github+json\r\n", (DWORD)-1L, WINHTTP_NO_REQUEST_DATA,
                                0, 0, 0) &&
        WinHttpReceiveResponse(r, nullptr)) {
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &size, WINHTTP_NO_HEADER_INDEX);
        if (status == 200) {
            body->clear();
            for (;;) {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(r, &avail) || avail == 0) break;
                size_t at = body->size();
                body->resize(at + avail);
                DWORD got = 0;
                if (!WinHttpReadData(r, &(*body)[at], avail, &got)) break;
                body->resize(at + got);
                if (body->size() > maxBytes) break;
            }
            ok = !body->empty() && body->size() <= maxBytes;
            if (!ok) *error = L"respuesta vacía o demasiado grande";
        } else {
            *error = Fmt(L"HTTP %lu", status);
        }
    } else {
        *error = Fmt(L"sin conexión (%lu)", GetLastError());
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}

// Minimal JSON string lookup: value of the first "key":"..." after `from`.
std::string JsonString(const std::string& json, const char* key, size_t from = 0) {
    std::string k = std::string("\"") + key + "\"";
    size_t p = json.find(k, from);
    if (p == std::string::npos) return {};
    p = json.find(':', p + k.size());
    if (p == std::string::npos) return {};
    p = json.find('"', p);
    if (p == std::string::npos) return {};
    size_t e = json.find('"', p + 1);
    return e == std::string::npos ? std::string() : json.substr(p + 1, e - p - 1);
}

std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }  // ASCII only

}  // namespace

std::wstring CurrentVersion() {
    wchar_t fake[32];
    if (GetEnvironmentVariableW(L"BATTERYHUB_FAKE_VERSION", fake, 32)) return fake;  // test hook
    return BH_VERSION_WSTR;
}

int CompareVersions(const std::wstring& a, const std::wstring& b) {
    auto parts = [](const std::wstring& v) {
        std::vector<int> n;
        size_t i = (!v.empty() && (v[0] == L'v' || v[0] == L'V')) ? 1 : 0;
        while (i < v.size() && n.size() < 4) {
            int x = 0;
            while (i < v.size() && iswdigit(v[i])) x = x * 10 + (v[i++] - L'0');
            n.push_back(x);
            if (i < v.size() && v[i] == L'.') ++i;
            else break;
        }
        while (n.size() < 4) n.push_back(0);
        return n;
    };
    auto x = parts(a), y = parts(b);
    for (int i = 0; i < 4; ++i)
        if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}

UpdateInfo CheckForUpdate() {
    UpdateInfo info;
    std::string json;
    std::wstring url = std::wstring(L"https://api.github.com/repos/") + kRepo + L"/releases/latest";
    if (!HttpGet(url, &json, &info.error, 512 * 1024)) return info;
    std::string tag = JsonString(json, "tag_name");
    if (tag.empty()) {
        info.error = L"respuesta sin versión";
        return info;
    }
    info.latest = Widen(tag[0] == 'v' || tag[0] == 'V' ? tag.substr(1) : tag);
    // The asset whose download URL ends in /BatteryHub.exe
    for (size_t p = 0; (p = json.find("\"browser_download_url\"", p)) != std::string::npos; ++p) {
        std::string u = JsonString(json, "browser_download_url", p);
        if (u.size() > 15 && u.compare(u.size() - 15, 15, "/BatteryHub.exe") == 0) {
            info.exeUrl = Widen(u);
            break;
        }
    }
    info.ok = true;
    info.newer = CompareVersions(info.latest, CurrentVersion()) > 0;
    return info;
}

bool InstallUpdate(const UpdateInfo& info, std::wstring* error) {
    if (info.exeUrl.empty()) {
        *error = L"el Release no tiene BatteryHub.exe";
        return false;
    }
    std::string bin;
    if (!HttpGet(info.exeUrl, &bin, error, 32 * 1024 * 1024)) return false;
    if (bin.size() < 64 * 1024 || bin[0] != 'M' || bin[1] != 'Z') {
        *error = L"la descarga no es un ejecutable válido";
        return false;
    }
    const std::wstring exe = ExePath(), fresh = exe + L".new", old = exe + L".old";
    HANDLE f = CreateFileW(fresh.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        *error = Fmt(L"no se puede escribir junto al programa (%lu)", GetLastError());
        return false;
    }
    DWORD written = 0;
    bool wrote = WriteFile(f, bin.data(), (DWORD)bin.size(), &written, nullptr) && written == bin.size();
    CloseHandle(f);
    if (!wrote) {
        DeleteFileW(fresh.c_str());
        *error = L"no se pudo guardar la descarga";
        return false;
    }
    // Swap: running exe -> .old, new -> exe. Roll back if the second move fails.
    DeleteFileW(old.c_str());
    if (!MoveFileExW(exe.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        *error = Fmt(L"no se pudo reemplazar el programa (%lu)", GetLastError());
        DeleteFileW(fresh.c_str());
        return false;
    }
    if (!MoveFileExW(fresh.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        *error = Fmt(L"no se pudo instalar la versión nueva (%lu)", GetLastError());
        MoveFileExW(old.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING);
        DeleteFileW(fresh.c_str());
        return false;
    }
    return true;
}

bool StartUpdatedApp() {
    std::wstring cmd = L"\"" + ExePath() + L"\" --after-update";
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

void DeleteOldBinary() { DeleteFileW((ExePath() + L".old").c_str()); }

}  // namespace bh
