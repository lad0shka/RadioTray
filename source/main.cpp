#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <shellapi.h>
#include <objbase.h>
#include <oleauto.h>
#include <winhttp.h>
#include <shlwapi.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <cwctype>

#include "vendor/bass/bass.h"
#include "embedded_bass.h"
#include "stream_metadata.h"
#include "version.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shlwapi.lib")

static const wchar_t* kAppName       = L"RadioTray";
static const wchar_t* kMutexName     = L"Local\\RadioTray.Singleton";
static const wchar_t* kPrevMutexName = L"Local\\AnimeRadio.Singleton";
static const wchar_t* kSettingsKey   = L"Software\\RadioTray";
static const wchar_t* kPrevKey       = L"Software\\AnimeRadio";
static const wchar_t* kLegacyKey     = L"Software\\Pulse\\PulseRadio";
static const wchar_t* kWindowClass   = L"RadioTrayHiddenWnd";
static const wchar_t* kRunKey        = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kRunValue      = L"RadioTray";

#define WM_TRAYICON    (WM_APP + 1)
#define WM_AUDIORESULT (WM_APP + 2)
#define WM_M3URESULT   (WM_APP + 3)
#define ID_WATCHDOG    1

// Context-menu command IDs. Station items use ID_MENU_STATION_BASE + index.
#define ID_MENU_STATION_BASE 1000
#define ID_MENU_AUTOSTART    1998
#define ID_MENU_EXIT         1999

static UINT g_taskbarCreated = 0;

struct Station { const wchar_t* id; const wchar_t* name; const wchar_t* url; int group = 0; };
static const Station kStations[] = {
    { L"listenmoe",  L"LISTEN.moe - JPOP",           L"https://listen.moe/fallback" },
    { L"animeradio", L"AnimeRadio.de",               L"https://www.animeradio.de/streams/hd.m3u" },
    { L"anison",     L"ANISON.FM",                    L"https://anison.fm/anison.m3u" },
    { L"kawaii",     L"J-Pop Powerplay Kawaii",       L"https://kathy.torontocast.com:3060/stream" },
    { L"animeamaze", L"AnimeAMAZE",                   L"http://icecast.d3mediaproductions.com:8000/AnimeAMAZE192" },
    { L"gensokyo",   L"Gensokyo Radio",               L"https://stream.gensokyoradio.net/3" },
    { L"jrock",      L"J-Rock Powerplay",             L"https://kathy.torontocast.com:3340/stream" },
    { L"sakura",     L"J-Pop Sakura",                 L"https://quincy.torontocast.com:2070/stream" },
    { L"powerplay",  L"J-Pop Powerplay",              L"https://kathy.torontocast.com:3560/stream" },
    { L"nr_nightride",   L"Nightride FM",           L"https://stream.nightride.fm/nightride.mp3",   1 },
    { L"nr_chillsynth",  L"Chillsynth FM",          L"https://stream.nightride.fm/chillsynth.mp3",  1 },
    { L"nr_datawave",    L"Datawave FM",            L"https://stream.nightride.fm/datawave.mp3",    1 },
    { L"nr_spacesynth",  L"Spacesynth FM",          L"https://stream.nightride.fm/spacesynth.mp3",  1 },
    { L"nr_darksynth",   L"Darksynth FM",           L"https://stream.nightride.fm/darksynth.mp3",   1 },
    { L"nr_horrorsynth", L"Horrorsynth FM",         L"https://stream.nightride.fm/horrorsynth.mp3", 1 },
    { L"nr_ebsm",        L"EBSM",                   L"https://stream.nightride.fm/ebsm.mp3",        1 },
    { L"coderadio",      L"Code Radio",             L"https://coderadio-admin-v2.freecodecamp.org/listen/coderadio/radio.mp3", 2 },
    { L"spacestation",   L"Space Station Soma",     L"https://somafm.com/m3u/spacestation.m3u",     2 },
    { L"synphaera",      L"Synphaera Radio",        L"https://somafm.com/m3u/synphaera.m3u",        2 },
    { L"groovesalad",    L"Groove Salad",           L"https://somafm.com/m3u/groovesalad.m3u",      2 },
};
static const int kStationCount = (int)(sizeof(kStations)/sizeof(kStations[0]));

// ---------- registry helpers ----------
static std::wstring ReadString(HKEY root, const std::wstring& sub, const wchar_t* name) {
    HKEY h;
    if (RegOpenKeyExW(root, sub.c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS) return L"";
    wchar_t buf[512]; DWORD sz = sizeof(buf); DWORD type = 0;
    std::wstring out;
    if (RegQueryValueExW(h, name, nullptr, &type, (BYTE*)buf, &sz) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ)) {
        out.assign(buf, sz / sizeof(wchar_t));
        while (!out.empty() && out.back() == 0) out.pop_back();
    }
    RegCloseKey(h);
    return out;
}
static DWORD ReadDword(HKEY root, const std::wstring& sub, const wchar_t* name, DWORD def) {
    HKEY h;
    if (RegOpenKeyExW(root, sub.c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS) return def;
    DWORD v = def, sz = sizeof(v), type = 0;
    RegQueryValueExW(h, name, nullptr, &type, (BYTE*)&v, &sz);
    RegCloseKey(h);
    return v;
}
static void WriteString(HKEY root, const std::wstring& sub, const wchar_t* name, const std::wstring& val) {
    HKEY h; DWORD disp = 0;
    if (RegCreateKeyExW(root, sub.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &h, &disp) != ERROR_SUCCESS) return;
    RegSetValueExW(h, name, 0, REG_SZ, (const BYTE*)val.c_str(), (DWORD)((val.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(h);
}
static std::wstring LoadStationId(const std::wstring& sub) {
    std::wstring id = ReadString(HKEY_CURRENT_USER, sub, L"StationId");
    for (int i = 0; i < kStationCount; ++i) if (id == kStations[i].id) return id;
    if (sub == kSettingsKey) {
        // One-time migration from the previous product key (kept intact).
        std::wstring prev = ReadString(HKEY_CURRENT_USER, kPrevKey, L"StationId");
        for (int i = 0; i < kStationCount; ++i) {
            if (prev == kStations[i].id) {
                WriteString(HKEY_CURRENT_USER, kSettingsKey, L"StationId", prev);
                return prev;
            }
        }
        // Older Pulse DWORD migration (0..3) also seeds the new primary.
        DWORD old = ReadDword(HKEY_CURRENT_USER, kLegacyKey, L"station", 0xFFFFFFFF);
        if (old < 4) {
            WriteString(HKEY_CURRENT_USER, kSettingsKey, L"StationId", kStations[old].id);
            return kStations[old].id;
        }
    }
    return kStations[0].id;
}
static void SaveStationId(const std::wstring& sub, const std::wstring& id) {
    WriteString(HKEY_CURRENT_USER, sub, L"StationId", id);
}
static void DeleteKeyTree(HKEY root, const std::wstring& sub) {
    HKEY h;
    if (RegOpenKeyExW(root, sub.c_str(), 0, DELETE | KEY_READ, &h) == ERROR_SUCCESS) {
        RegDeleteValueW(h, L"StationId");
        RegCloseKey(h);
        RegDeleteKeyW(root, sub.c_str());
    }
}

// ---------- start with Windows (HKCU Run entry) ----------
static bool IsAutostartEnabled() {
    return !ReadString(HKEY_CURRENT_USER, kRunKey, kRunValue).empty();
}
static bool SetAutostart(bool enable) {
    HKEY h = nullptr;
    if (!enable) {
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &h) != ERROR_SUCCESS) return true;
        LONG rc = RegDeleteValueW(h, kRunValue);
        RegCloseKey(h);
        return rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND;
    }
    wchar_t path[32768];
    DWORD n = GetModuleFileNameW(nullptr, path, (DWORD)(sizeof(path) / sizeof(path[0])));
    if (n == 0 || n >= sizeof(path) / sizeof(path[0])) return false;
    const std::wstring cmd = std::wstring(L"\"") + path + L"\"";
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &h, nullptr) != ERROR_SUCCESS)
        return false;
    LONG rc = RegSetValueExW(h, kRunValue, 0, REG_SZ, (const BYTE*)cmd.c_str(),
                             (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(h);
    return rc == ERROR_SUCCESS;
}

// ---------- icon rendering (anti-aliased 32bpp premultiplied) ----------
static bool InsideTriangle(double x, double y) {
    const double ax = 0.22, ay = 0.10, bx = 0.22, by = 0.90, cx = 0.82, cy = 0.50;
    auto sign = [](double px, double py, double qx, double qy, double rx, double ry) {
        return (px - rx) * (qy - ry) - (qx - rx) * (py - ry);
    };
    double d1 = sign(x, y, ax, ay, bx, by), d2 = sign(x, y, bx, by, cx, cy), d3 = sign(x, y, cx, cy, ax, ay);
    bool neg = (d1 < 0) || (d2 < 0) || (d3 < 0), pos = (d1 > 0) || (d2 > 0) || (d3 > 0);
    return !(neg && pos);
}
static HICON RenderIcon(bool stop) {
    const int N = 32;
    BITMAPV5HEADER bi = {};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = N; bi.bV5Height = -N; bi.bV5Planes = 1; bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000; bi.bV5GreenMask = 0x0000FF00; bi.bV5BlueMask = 0x000000FF; bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC hdc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(hdc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, hdc);
    if (!color || !bits) { if (color) DeleteObject(color); return nullptr; }
    unsigned char* px = (unsigned char*)bits;
    ZeroMemory(px, (size_t)N * N * 4);
    const int SS = 8; const double inv = 1.0 / SS;
    const double cr = 94.0, cg = 230.0, cb = 181.0;
    for (int y = 0; y < N; ++y) {
        for (int x = 0; x < N; ++x) {
            int cnt = 0;
            for (int sy = 0; sy < SS; ++sy) for (int sx = 0; sx < SS; ++sx) {
                double fx = (x + (sx + 0.5) * inv) / N;
                double fy = (y + (sy + 0.5) * inv) / N;
                bool in = stop ? (fx >= 0.20 && fx <= 0.80 && fy >= 0.20 && fy <= 0.80) : InsideTriangle(fx, fy);
                if (in) ++cnt;
            }
            double cov = (double)cnt / (SS * SS);
            unsigned char* p = px + ((size_t)(y * N + x) * 4);
            p[0] = (unsigned char)(cb * cov + 0.5);
            p[1] = (unsigned char)(cg * cov + 0.5);
            p[2] = (unsigned char)(cr * cov + 0.5);
            p[3] = (unsigned char)(cov * 255.0 + 0.5);
        }
    }
    HBITMAP mask = CreateBitmap(N, N, 1, 1, nullptr);
    BYTE mb[128] = { 0 };
    if (mask) SetBitmapBits(mask, sizeof(mb), mb);
    ICONINFO ii = {};
    ii.fIcon = TRUE; ii.hbmColor = color; ii.hbmMask = mask;
    HICON hIcon = CreateIconIndirect(&ii);
    DeleteObject(color);
    if (mask) DeleteObject(mask);
    return hIcon;
}

// ---------- cross-thread bridge ----------
struct Bridge {
    HWND hwnd = nullptr;
    std::mutex mt;
    bool closed = false;
    void SetHwnd(HWND h) { std::lock_guard<std::mutex> lk(mt); hwnd = h; }
    void Close() { std::lock_guard<std::mutex> lk(mt); closed = true; hwnd = nullptr; }
    bool Post(UINT msg, WPARAM wp, LPARAM lp) {
        std::lock_guard<std::mutex> lk(mt);
        if (closed || !hwnd) return false;
        return PostMessageW(hwnd, msg, wp, lp) != FALSE;
    }
};

struct M3UResult { uint64_t gen; std::wstring url; std::wstring error; };

// ---------- audio runtime (BASS) ----------
// Reference-counted so that pending open jobs and adopted streams always keep
// the runtime alive; BASS_Free only runs when the last owner lets go.
class AudioRuntime {
    std::mutex m_mt;
    bool m_ok = false;
public:
    bool EnsureInitialized() {
        std::lock_guard<std::mutex> lk(m_mt);
        if (m_ok) return true;
        if (!EnsureEmbeddedBass()) return false;
        if (HIWORD(BASS_GetVersion()) != BASSVERSION) return false;
        if (!BASS_Init(-1, 44100, 0, nullptr, nullptr)) return false;
        if (!BASS_SetConfig(BASS_CONFIG_NET_BUFFER,      30000) ||
            !BASS_SetConfig(BASS_CONFIG_NET_PREBUF,      0) ||
            !BASS_SetConfig(BASS_CONFIG_BUFFER,          1000) ||
            !BASS_SetConfig(BASS_CONFIG_NET_TIMEOUT,     10000) ||
            !BASS_SetConfig(BASS_CONFIG_NET_READTIMEOUT, 15000)) {
            BASS_Free();
            return false;
        }
        m_ok = true;
        return true;
    }
    ~AudioRuntime() {
        std::lock_guard<std::mutex> lk(m_mt);
        if (m_ok) { BASS_Free(); m_ok = false; }
    }
};

static std::mutex g_rtMutex;
static std::shared_ptr<AudioRuntime> g_rtStrong;
static std::shared_ptr<AudioRuntime> AcquireRuntime() {
    std::lock_guard<std::mutex> lk(g_rtMutex);
    if (g_rtStrong) return g_rtStrong;
    auto sp = std::make_shared<AudioRuntime>();
    if (!sp->EnsureInitialized()) return nullptr;
    g_rtStrong = sp;
    return sp;
}

// Stable per-attempt cancel token. Never passed to a different request.
struct StreamToken { std::atomic<bool> cancelled{ false }; };

// Result payload: frees the stream via RAII unless ownership is transferred.
class AudioStreamResult {
public:
    uint64_t gen = 0;
    HSTREAM  stream = 0;
    std::shared_ptr<AudioRuntime> runtime;
    ~AudioStreamResult() {
        if (stream) { BASS_StreamFree(stream); stream = 0; }
    }
};


static void PostAudioError(uint64_t gen, const std::shared_ptr<Bridge>& bridge,
                           const std::shared_ptr<AudioRuntime>& runtime) {
    auto* res = new (std::nothrow) AudioStreamResult();
    if (!res) return;
    res->gen = gen;
    res->stream = 0;
    res->runtime = runtime;
    if (!bridge->Post(WM_AUDIORESULT, 0, (LPARAM)res)) delete res;
}

// Runs BASS_StreamCreateURL off the UI thread. Never Play here: the UI thread
// adopts the handle only when the generation still matches and playback is
// still desired.
static void OpenStreamAsync(const std::wstring& url, uint64_t gen,
                            std::shared_ptr<Bridge> bridge,
                            std::shared_ptr<AudioRuntime> runtime,
                            std::shared_ptr<StreamToken> token) {
    if (!runtime || !bridge || !token) return;
    try {
        std::thread([url, gen, bridge, runtime, token]() {
            if (token->cancelled.load()) return;
            HSTREAM h = BASS_StreamCreateURL(
                reinterpret_cast<const char*>(url.c_str()), 0,
                BASS_STREAM_BLOCK | BASS_UNICODE, nullptr, token.get());
            if (token->cancelled.load()) {
                if (h) BASS_StreamFree(h);
                return;
            }
            if (!h) { PostAudioError(gen, bridge, runtime); return; }
            auto* res = new (std::nothrow) AudioStreamResult();
            if (!res) { BASS_StreamFree(h); return; }
            res->gen = gen;
            res->stream = h;
            res->runtime = runtime;
            if (!bridge->Post(WM_AUDIORESULT, 0, (LPARAM)res)) delete res;
        }).detach();
    } catch (...) {
        PostAudioError(gen, bridge, runtime);
    }
}

// ---------- M3U resolution ----------
static bool IsM3U(const std::wstring& url) {
    std::wstring u = url;
    size_t q = u.find(L'?');
    if (q != std::wstring::npos) u = u.substr(0, q);
    if (u.size() >= 4) {
        std::wstring e = u.substr(u.size() - 4);
        for (auto& c : e) c = (wchar_t)towlower(c);
        if (e == L".m3u") return true;
    }
    if (u.size() >= 5) {
        std::wstring e = u.substr(u.size() - 5);
        for (auto& c : e) c = (wchar_t)towlower(c);
        if (e == L".m3u8") return true;
    }
    return false;
}
static bool HttpGet(const std::wstring& url, std::atomic<bool>& cancel, std::string& out, std::wstring& err, std::wstring& finalUrl) {
    URL_COMPONENTS uc = {}; uc.dwStructSize = sizeof(uc);
    wchar_t host[512] = { 0 }, path[4096] = { 0 }, extra[4096] = { 0 };
    uc.lpszHostName = host; uc.dwHostNameLength = 512;
    uc.lpszUrlPath = path;  uc.dwUrlPathLength = 4096;
    uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 4096;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) { err = L"bad url"; return false; }
    if (uc.nScheme != INTERNET_SCHEME_HTTP && uc.nScheme != INTERNET_SCHEME_HTTPS) { err = L"bad scheme"; return false; }
    if (!path[0]) wcscpy_s(path, L"/");
    std::wstring reqPath = path;
    reqPath += extra;
    HINTERNET s = WinHttpOpen(L"RadioTray/" RT_VERSION_WSTR, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) { err = L"open"; return false; }
    int t = 10000;
    WinHttpSetTimeouts(s, t, t, t, t);
    HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
    if (!c) { WinHttpCloseHandle(s); err = L"connect"; return false; }
    DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET r = WinHttpOpenRequest(c, L"GET", reqPath.c_str(), nullptr, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!r) { WinHttpCloseHandle(c); WinHttpCloseHandle(s); err = L"request"; return false; }
    bool ok = false;
    DWORD startTick = GetTickCount();
    if (cancel.load()) {
        err = L"cancel";
    } else if (WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
               WinHttpReceiveResponse(r, nullptr)) {
        DWORD status = 0, sl = sizeof(status);
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &sl, WINHTTP_NO_HEADER_INDEX);
        if (status == 200) {
            ok = true;
            DWORD declaredLen = 0, dlSize = sizeof(declaredLen);
            bool hasLen = WinHttpQueryHeaders(r,
                WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &declaredLen, &dlSize, WINHTTP_NO_HEADER_INDEX) == TRUE;
            if (hasLen && declaredLen > 65536) { err = L"too big"; ok = false; }
            for (; ok;) {
                if (cancel.load()) { err = L"cancel"; ok = false; break; }
                if (GetTickCount() - startTick > 30000) { err = L"timeout"; ok = false; break; }
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(r, &avail)) { err = L"read"; ok = false; break; }
                if (avail == 0) break;
                char buf[4096];
                DWORD want = avail > sizeof(buf) ? (DWORD)sizeof(buf) : avail;
                DWORD rd = 0;
                if (!WinHttpReadData(r, buf, want, &rd)) { err = L"read"; ok = false; break; }
                if (rd == 0) break;
                if (out.size() + rd > 65536) { err = L"too big"; ok = false; break; }
                out.append(buf, rd);
            }
            if (ok && hasLen && out.size() != (size_t)declaredLen) { err = L"incomplete body"; ok = false; }
        } else {
            err = L"http " + std::to_wstring((unsigned)status);
        }
    } else {
        err = L"recv";
    }
    if (ok) {
        DWORD nl = 0;
        WinHttpQueryOption(r, WINHTTP_OPTION_URL, nullptr, &nl);
        if (nl > 0) {
            std::wstring buf(nl / sizeof(wchar_t) + 1, 0);
            DWORD cap = (DWORD)(buf.size() * sizeof(wchar_t));
            if (WinHttpQueryOption(r, WINHTTP_OPTION_URL, &buf[0], &cap)) {
                while (!buf.empty() && buf.back() == 0) buf.pop_back();
                finalUrl = buf;
            }
        }
        if (finalUrl.empty()) finalUrl = url;
    }
    WinHttpCloseHandle(r);
    WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}
static std::wstring ParseM3U(const std::string& body, const std::wstring& baseUrl) {
    int wl = MultiByteToWideChar(CP_UTF8, 0, body.data(), (int)body.size(), nullptr, 0);
    std::wstring w(wl > 0 ? wl : 0, 0);
    if (wl > 0) MultiByteToWideChar(CP_UTF8, 0, body.data(), (int)body.size(), &w[0], wl);
    if (!w.empty() && w[0] == 0xFEFF) w.erase(0, 1);
    size_t pos = 0;
    while (pos < w.size()) {
        size_t eol = w.find_first_of(L"\r\n", pos);
        std::wstring line = (eol == std::wstring::npos) ? w.substr(pos) : w.substr(pos, eol - pos);
        pos = (eol == std::wstring::npos) ? w.size() : eol + 1;
        size_t b = line.find_first_not_of(L" \t");
        if (b == std::wstring::npos) continue;
        size_t e = line.find_last_not_of(L" \t");
        line = line.substr(b, e - b + 1);
        if (line.empty() || line[0] == L'#') continue;
        {
            URL_COMPONENTS uc = {}; uc.dwStructSize = sizeof(uc);
            if (WinHttpCrackUrl(line.c_str(), 0, 0, &uc) &&
                (uc.nScheme == INTERNET_SCHEME_HTTP || uc.nScheme == INTERNET_SCHEME_HTTPS)) return line;
        }
        wchar_t comb[4096]; DWORD cl = 4096;
        if (SUCCEEDED(UrlCombineW(baseUrl.c_str(), line.c_str(), comb, &cl, 0))) {
            URL_COMPONENTS uc = {}; uc.dwStructSize = sizeof(uc);
            if (WinHttpCrackUrl(comb, 0, 0, &uc) &&
                (uc.nScheme == INTERNET_SCHEME_HTTP || uc.nScheme == INTERNET_SCHEME_HTTPS)) return comb;
        }
    }
    return L"";
}
static void ResolveM3UAsync(const std::wstring& url, uint64_t gen,
                            std::shared_ptr<Bridge> bridge,
                            std::shared_ptr<std::atomic<bool>> cancel) {
    std::thread([url, gen, bridge, cancel]() {
        std::string body;
        std::wstring err;
        std::wstring finalUrl;
        bool ok = HttpGet(url, *cancel, body, err, finalUrl);
        auto* res = new (std::nothrow) M3UResult{ gen, L"", L"" };
        if (!res) return;
        if (!ok) {
            res->error = err.empty() ? L"error" : err;
        } else {
            std::wstring stream = ParseM3U(body, finalUrl.empty() ? url : finalUrl);
            if (stream.empty()) res->error = L"no stream"; else res->url = stream;
        }
        if (!bridge->Post(WM_M3URESULT, 0, (LPARAM)res)) delete res;
    }).detach();
}

// ---------- application ----------
class App {
public:
    enum State { Stopped, Connecting, Playing, Error };
private:
    HWND m_hwnd = nullptr;
    std::shared_ptr<Bridge> m_bridge;
    HICON m_playIcon = nullptr, m_stopIcon = nullptr, m_curIcon = nullptr;
    bool m_trayAdded = false;
    int m_sel = 0;
    bool m_desired = false;
    State m_state = Stopped;
    uint64_t m_gen = 0;
    std::shared_ptr<AudioRuntime> m_runtime;
    std::shared_ptr<StreamToken> m_token;
    HSTREAM m_stream = 0;
    std::wstring m_subkey;
    DWORD m_attemptStart = 0;
    bool m_ballooned = false;
    std::shared_ptr<std::atomic<bool>> m_m3uCancel;
    bool m_muted = false;
    std::wstring m_trackTitle;
    DWORD m_lastMetaPoll = 0;
    std::wstring m_lastTooltip;
    HICON m_lastIcon = nullptr;

    void AddTray() {
        if (!m_hwnd) return;
        NOTIFYICONDATAW nid = {}; nid.cbSize = sizeof(nid); nid.hWnd = m_hwnd; nid.uID = 1;
        nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        nid.uCallbackMessage = WM_TRAYICON;
        nid.hIcon = m_curIcon ? m_curIcon : m_playIcon;
        wcsncpy_s(nid.szTip, kAppName, _TRUNCATE);
        Shell_NotifyIconW(NIM_ADD, &nid);
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
        m_trayAdded = true;
        m_lastTooltip.clear();
        m_lastIcon = nullptr;
        UpdateTray();
    }
    const wchar_t* StateWord() const {
        switch (m_state) {
            case Stopped:    return L"Paused";
            case Connecting: return L"Connecting";
            case Playing:    return L"Playing";
            case Error:      return L"Error";
        }
        return L"Paused";
    }
    void UpdateTray() {
        HICON want = (m_state == Stopped || m_state == Error) ? m_playIcon : m_stopIcon;
        if (want) m_curIcon = want;
        if (!m_trayAdded) return;
        std::wstring tip = streammeta::BuildTrayTooltip(kStations[m_sel].name, m_trackTitle);
        if (tip == m_lastTooltip && m_curIcon == m_lastIcon) return;
        m_lastTooltip = tip;
        m_lastIcon = m_curIcon;
        NOTIFYICONDATAW nid = {}; nid.cbSize = sizeof(nid); nid.hWnd = m_hwnd; nid.uID = 1;
        nid.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        nid.hIcon = m_curIcon;
        wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
    void ToggleAutostart() {
        if (!SetAutostart(!IsAutostartEnabled()))
            Balloon(kAppName, L"Could not change the startup setting");
    }
    void Balloon(const wchar_t* title, const wchar_t* body) {
        if (!m_trayAdded) return;
        NOTIFYICONDATAW nid = {}; nid.cbSize = sizeof(nid); nid.hWnd = m_hwnd; nid.uID = 1;
        nid.uFlags = NIF_INFO;
        nid.dwInfoFlags = NIIF_INFO;
        wcsncpy_s(nid.szInfoTitle, title, _TRUNCATE);
        wcsncpy_s(nid.szInfo, body, _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }

    void FreeAdoptedStream() {
        if (m_stream) { BASS_StreamFree(m_stream); m_stream = 0; }
        m_trackTitle.clear();
        m_lastMetaPoll = 0;
    }
    void RefreshMetadata() {
        if (!m_stream) return;
        DWORD now = GetTickCount();
        if ((DWORD)(now - m_lastMetaPoll) < 1000) return;
        m_lastMetaPoll = now;
        std::wstring title = streammeta::ReadStreamTitle(m_stream);
        if (title != m_trackTitle) {
            m_trackTitle = title;
            UpdateTray();
        }
    }
    void CancelToken() {
        if (m_token) {
            m_token->cancelled.store(true);
            BASS_StreamCancel(m_token.get());
        }
        m_token.reset();
    }
    void StartStream(const std::wstring& url, uint64_t gen) {
        if (gen != m_gen) return;
        if (!m_runtime) { OnPlaybackError(); return; }
        auto token = std::make_shared<StreamToken>();
        m_token = token;
        OpenStreamAsync(url, gen, m_bridge, m_runtime, token);
    }
    void OnPlaybackError() {
        if (m_state == Stopped) return;
        m_desired = false;
        m_state = Error;
        if (m_m3uCancel) m_m3uCancel->store(true);
        CancelToken();
        FreeAdoptedStream();
        ++m_gen;
        KillTimer(m_hwnd, ID_WATCHDOG);
        UpdateTray();
        if (!m_ballooned) { m_ballooned = true; Balloon(kAppName, L"Playback error"); }
    }
    void StopAll() {
        m_desired = false;
        m_state = Stopped;
        ++m_gen;
        if (m_m3uCancel) m_m3uCancel->store(true);
        CancelToken();
        FreeAdoptedStream();
        KillTimer(m_hwnd, ID_WATCHDOG);
        UpdateTray();
    }
    void StartUrl(const std::wstring& url) {
        ++m_gen;
        m_desired = true;
        m_state = Connecting;
        m_ballooned = false;
        if (m_m3uCancel) m_m3uCancel->store(true);
        CancelToken();
        FreeAdoptedStream();
        KillTimer(m_hwnd, ID_WATCHDOG);
        m_m3uCancel = std::make_shared<std::atomic<bool>>(false);
        m_attemptStart = GetTickCount();
        SetTimer(m_hwnd, ID_WATCHDOG, 100, nullptr);
        UpdateTray();
        if (IsM3U(url)) ResolveM3UAsync(url, m_gen, m_bridge, m_m3uCancel);
        else StartStream(url, m_gen);
    }
    void Toggle() {
        if (m_desired) StopAll();
        else StartUrl(kStations[m_sel].url);
    }
    void SelectStation(int i) {
        if (i < 0 || i >= kStationCount) return;
        m_sel = i;
        SaveStationId(m_subkey, kStations[i].id);
        StartUrl(kStations[i].url);
    }
    void ShowMenu() {
        HMENU menu = BuildMenu();
        POINT pt; GetCursorPos(&pt);
        SetForegroundWindow(m_hwnd);
        int cmd = (int)TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, m_hwnd, nullptr);
        PostMessageW(m_hwnd, WM_NULL, 0, 0);
        DestroyMenu(menu);
        if (cmd) SendMessageW(m_hwnd, WM_COMMAND, (WPARAM)cmd, 0);
    }
    void OnWatchdog() {
        if (m_desired && m_stream) RefreshMetadata();
        DWORD now = GetTickCount();
        if (m_state == Connecting) {
            if (m_stream) {
                DWORD st = BASS_ChannelIsActive(m_stream);
                if (st == BASS_ACTIVE_PLAYING && MediaTime() > 0.1) {
                    m_state = Playing;
                    UpdateTray();
                    return;
                }
                if (st == BASS_ACTIVE_STOPPED) { OnPlaybackError(); return; }
            }
            if (now - m_attemptStart > 30000) OnPlaybackError();
            return;
        }
        if (m_state == Playing) {
            if (!m_stream) return;
            DWORD st = BASS_ChannelIsActive(m_stream);
            if (st == BASS_ACTIVE_STOPPED) { OnPlaybackError(); return; }
            // BASS_ACTIVE_STALLED is transient: keep desired and let BASS
            // resume via BASS_ATTRIB_NET_RESUME without re-calling Play.
        }
    }
    void HandleAudioResult(AudioStreamResult& r) {
        if (r.gen != m_gen || !m_desired) return;
        if (!r.stream) { OnPlaybackError(); return; }
        FreeAdoptedStream();
        m_stream = r.stream;
        r.stream = 0; // ownership transferred; RAII must not free it
        float vol = m_muted ? 0.0f : 0.5f;
        if (!BASS_ChannelSetAttribute(m_stream, BASS_ATTRIB_VOL, vol) ||
            !BASS_ChannelSetAttribute(m_stream, BASS_ATTRIB_NET_RESUME, 10.0f)) {
            OnPlaybackError();
            return;
        }
        if (!BASS_ChannelPlay(m_stream, FALSE)) { OnPlaybackError(); return; }
    }
    void HandleM3UResult(const M3UResult& r) {
        if (r.gen != m_gen || !m_desired) return;
        if (!r.error.empty()) { OnPlaybackError(); return; }
        StartStream(r.url, r.gen);
    }
    void Cleanup() {
        KillTimer(m_hwnd, ID_WATCHDOG);
        if (m_trayAdded && m_hwnd) {
            NOTIFYICONDATAW nid = {}; nid.cbSize = sizeof(nid); nid.hWnd = m_hwnd; nid.uID = 1;
            Shell_NotifyIconW(NIM_DELETE, &nid);
            m_trayAdded = false;
        }
        if (m_bridge) m_bridge->Close();
        ++m_gen;
        if (m_m3uCancel) m_m3uCancel->store(true);
        CancelToken();
        FreeAdoptedStream();
        MSG msg;
        while (m_hwnd && PeekMessageW(&msg, m_hwnd, WM_AUDIORESULT, WM_AUDIORESULT, PM_REMOVE))
            delete (AudioStreamResult*)msg.lParam;
        while (m_hwnd && PeekMessageW(&msg, m_hwnd, WM_M3URESULT, WM_M3URESULT, PM_REMOVE))
            delete (M3UResult*)msg.lParam;
        m_runtime.reset();
    }

    static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
        if (msg == WM_NCCREATE) {
            auto cs = (CREATESTRUCTW*)l;
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
            return DefWindowProcW(h, msg, w, l);
        }
        App* app = (App*)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (app) return app->OnMessage(h, msg, w, l);
        return DefWindowProcW(h, msg, w, l);
    }
    LRESULT OnMessage(HWND h, UINT msg, WPARAM w, LPARAM l) {
        if (g_taskbarCreated && msg == g_taskbarCreated) { AddTray(); return 0; }
        switch (msg) {
            case WM_TRAYICON: {
                UINT evt = LOWORD(l);
                if (evt == NIN_SELECT || evt == NIN_KEYSELECT) Toggle();
                else if (evt == WM_CONTEXTMENU) ShowMenu();
                return 0;
            }
            case WM_AUDIORESULT: {
                auto* r = (AudioStreamResult*)l;
                if (r) { HandleAudioResult(*r); delete r; }
                return 0;
            }
            case WM_M3URESULT: {
                auto* r = (M3UResult*)l;
                if (r) { HandleM3UResult(*r); delete r; }
                return 0;
            }
            case WM_COMMAND: {
                UINT id = LOWORD(w);
                if (id >= (UINT)ID_MENU_STATION_BASE && id < (UINT)(ID_MENU_STATION_BASE + kStationCount))
                    SelectStation((int)id - ID_MENU_STATION_BASE);
                else if (id == ID_MENU_AUTOSTART) ToggleAutostart();
                else if (id == ID_MENU_EXIT) DoExit();
                return 0;
            }
            case WM_TIMER:
                if (w == ID_WATCHDOG) OnWatchdog();
                return 0;
            case WM_DESTROY:
                Cleanup();
                m_hwnd = nullptr;
                PostQuitMessage(0);
                return 0;
        }
        return DefWindowProcW(h, msg, w, l);
    }

public:
    ~App() { Shutdown(); }

    bool Create(const std::wstring& subkey, bool muted) {
        m_subkey = subkey;
        m_muted = muted;
        if (!g_taskbarCreated) g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        static bool registered = false;
        if (!registered) {
            WNDCLASSEXW wc = {}; wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = WndProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = kWindowClass;
            if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
            registered = true;
        }
        m_runtime = AcquireRuntime();
        if (!m_runtime) return false;
        m_bridge = std::make_shared<Bridge>();
        m_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, kAppName, WS_POPUP,
                                 0, 0, 0, 0, nullptr, nullptr, GetModuleHandleW(nullptr), this);
        if (!m_hwnd) return false;
        m_bridge->SetHwnd(m_hwnd);
        std::wstring id = LoadStationId(m_subkey);
        for (int i = 0; i < kStationCount; ++i) if (id == kStations[i].id) { m_sel = i; break; }
        m_playIcon = RenderIcon(false);
        m_stopIcon = RenderIcon(true);
        m_curIcon = m_playIcon;
        AddTray();
        return true;
    }
    void Run() {
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    void PumpOnce() {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) continue;
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
    }
    void Shutdown() {
        if (m_hwnd) { DestroyWindow(m_hwnd); m_hwnd = nullptr; }
        if (m_playIcon) { DestroyIcon(m_playIcon); m_playIcon = nullptr; }
        if (m_stopIcon) { DestroyIcon(m_stopIcon); m_stopIcon = nullptr; }
        m_curIcon = nullptr;
    }
    void DoExit() { if (m_hwnd) DestroyWindow(m_hwnd); }

    HWND hwnd() const { return m_hwnd; }
    State GetState() const { return m_state; }
    bool GetDesired() const { return m_desired; }
    int GetSelected() const { return m_sel; }
    bool HasTray() const { return m_trayAdded; }
    HICON PlayIcon() const { return m_playIcon; }
    HICON StopIcon() const { return m_stopIcon; }
    HSTREAM StreamHandle() const { return m_stream; }
    bool MediaHasAudio() const { return m_stream != 0; }
    double MediaTime() const {
        if (!m_stream) return -1.0;
        QWORD pos = BASS_ChannelGetPosition(m_stream, BASS_POS_BYTE);
        if (pos == (QWORD)-1) return -1.0;
        double t = BASS_ChannelBytes2Seconds(m_stream, pos);
        return t < 0 ? -1.0 : t;
    }
    void StopNow() { StopAll(); }
    void StartUrlPublic(const std::wstring& url) { StartUrl(url); }
    HMENU BuildMenu() {
        HMENU menu = CreatePopupMenu();
        if (!menu) return nullptr;
        struct MenuGroup { const wchar_t* title; int group; };
        static const MenuGroup groups[] = {
            { L"J-POP / J-ROCK", 0 },
            { L"CHILLSYNTH",     1 },
            { L"MUSIC FOR WORK", 2 },
        };
        for (const auto& g : groups) {
            HMENU sub = CreatePopupMenu();
            if (!sub) { DestroyMenu(menu); return nullptr; }
            for (int i = 0; i < kStationCount; ++i) {
                if (kStations[i].group != g.group) continue;
                AppendMenuW(sub, MF_STRING, (UINT_PTR)(ID_MENU_STATION_BASE + i), kStations[i].name);
            }
            if (m_sel >= 0 && m_sel < kStationCount && kStations[m_sel].group == g.group) {
                const UINT cur = (UINT)(ID_MENU_STATION_BASE + m_sel);
                CheckMenuRadioItem(sub, cur, cur, cur, MF_BYCOMMAND);
            }
            if (!AppendMenuW(menu, MF_POPUP, (UINT_PTR)sub, g.title)) {
                DestroyMenu(sub);
                DestroyMenu(menu);
                return nullptr;
            }
        }
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (IsAutostartEnabled() ? MF_CHECKED : 0),
                    ID_MENU_AUTOSTART, L"Start with Windows");
        AppendMenuW(menu, MF_STRING, ID_MENU_EXIT, L"Exit");
        return menu;
    }
};

// ---------- test helpers ----------
static bool WaitFor(std::function<bool()> cond, DWORD ms, App& app) {
    DWORD start = GetTickCount();
    while (GetTickCount() - start < ms) {
        app.PumpOnce();
        if (cond()) return true;
        Sleep(10);
    }
    app.PumpOnce();
    return cond();
}

struct TestResult { std::wstring name; bool pass; std::wstring detail; };

static std::wstring JsonEscape(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) {
        switch (c) {
            case L'\\': o += L"\\\\"; break;
            case L'"':  o += L"\\\""; break;
            case L'\n': o += L"\\n"; break;
            case L'\r': o += L"\\r"; break;
            case L'\t': o += L"\\t"; break;
            default:
                if (c < 0x20) { wchar_t b[8]; swprintf_s(b, L"\\u%04x", (unsigned)c); o += b; }
                else o += c;
        }
    }
    return o;
}
static void WriteReport(const std::wstring& path, const std::vector<TestResult>& items, bool all) {
    std::wstring j = L"{\n  \"pass\": ";
    j += all ? L"true" : L"false";
    j += L",\n  \"items\": [\n";
    for (size_t i = 0; i < items.size(); ++i) {
        j += L"    {\"name\": \"" + JsonEscape(items[i].name) + L"\", \"pass\": ";
        j += items[i].pass ? L"true" : L"false";
        j += L", \"detail\": \"" + JsonEscape(items[i].detail) + L"\"}";
        if (i + 1 < items.size()) j += L",";
        j += L"\n";
    }
    j += L"  ]\n}\n";
    int n = WideCharToMultiByte(CP_UTF8, 0, j.c_str(), (int)j.size(), nullptr, 0, nullptr, nullptr);
    std::string u(n > 0 ? n : 0, 0);
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, j.c_str(), (int)j.size(), &u[0], n, nullptr, nullptr);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) { DWORD w = 0; WriteFile(h, u.data(), (DWORD)u.size(), &w, nullptr); CloseHandle(h); }
}

static int RunSelfTest(const std::wstring& reportPath) {
    std::vector<TestResult> items;
    auto add = [&](const std::wstring& n, bool p, const std::wstring& d) { items.push_back({ n, p, d }); };
    std::wstring sub = L"Software\\RadioTray\\selftest_" + std::to_wstring(GetCurrentProcessId()) +
                       L"_" + std::to_wstring(GetTickCount64());
    std::wstring persistedId;

    {
        App app;
        if (!app.Create(sub, true)) {
            add(L"init", false, L"create failed");
            WriteReport(reportPath, items, false);
            DeleteKeyTree(HKEY_CURRENT_USER, sub);
            return 1;
        }

        bool stopped = app.GetState() == App::Stopped && !app.GetDesired();
        bool hidden = app.hwnd() && !IsWindowVisible(app.hwnd());
        add(L"startup", stopped && hidden, L"");

        HMENU m = app.BuildMenu();
        bool menuOk = (m != nullptr);
        std::wstring menuDetail;
        if (!m) {
            menuDetail = L"null";
        } else {
            struct ExpectedGroup { const wchar_t* title; int group; int count; };
            const ExpectedGroup exp[] = {
                { L"J-POP / J-ROCK", 0, 9 },
                { L"CHILLSYNTH",     1, 7 },
                { L"MUSIC FOR WORK", 2, 4 },
            };
            int rootCount = GetMenuItemCount(m);
            if (rootCount != 6) { menuOk = false; menuDetail += L"root=" + std::to_wstring(rootCount) + L";"; }
            for (int g = 0; g < 3; ++g) {
                if (!(GetMenuState(m, g, MF_BYPOSITION) & MF_POPUP)) { menuOk = false; menuDetail += L"noPopup" + std::to_wstring(g) + L";"; }
            }
            if (!(GetMenuState(m, 3, MF_BYPOSITION) & MF_SEPARATOR)) { menuOk = false; menuDetail += L"noSep;"; }
            if (GetMenuItemID(m, 4) != ID_MENU_AUTOSTART) { menuOk = false; menuDetail += L"noAutostart;"; }
            if (GetMenuItemID(m, 5) != ID_MENU_EXIT) { menuOk = false; menuDetail += L"noExit;"; }
            int checkedCount = 0;
            for (int g = 0; g < 3; ++g) {
                HMENU sub = GetSubMenu(m, g);
                if (!sub) { menuOk = false; menuDetail += L"noSub" + std::to_wstring(g) + L";"; continue; }
                wchar_t title[128] = { 0 };
                GetMenuStringW(m, g, title, 128, MF_BYPOSITION);
                if (wcscmp(title, exp[g].title) != 0) { menuOk = false; menuDetail += L"title" + std::to_wstring(g) + L";"; }
                int subCount = GetMenuItemCount(sub);
                if (subCount != exp[g].count) { menuOk = false; menuDetail += L"count" + std::to_wstring(g) + L"=" + std::to_wstring(subCount) + L";"; }
                int pos = 0;
                for (int i = 0; i < kStationCount; ++i) {
                    if (kStations[i].group != exp[g].group) continue;
                    if (pos >= subCount) { menuOk = false; break; }
                    UINT cmd = GetMenuItemID(sub, pos);
                    if (cmd != (UINT)(1000 + i)) {
                        menuOk = false;
                        menuDetail += L"id" + std::to_wstring(g) + L":" + std::to_wstring(cmd) + L";";
                    } else if (i == app.GetSelected()) {
                        if (GetMenuState(sub, cmd, MF_BYCOMMAND) & MF_CHECKED) ++checkedCount;
                        else { menuOk = false; menuDetail += L"notChecked;"; }
                    } else if (GetMenuState(sub, cmd, MF_BYCOMMAND) & MF_CHECKED) {
                        menuOk = false;
                        menuDetail += L"extraChecked;";
                    }
                    ++pos;
                }
                if (pos != exp[g].count) menuOk = false;
            }
            if (checkedCount != 1) { menuOk = false; menuDetail += L"checked=" + std::to_wstring(checkedCount) + L";"; }
            DestroyMenu(m);
        }
        add(L"menu", menuOk, menuDetail);

        SendMessageW(app.hwnd(), WM_TRAYICON, 0, MAKELPARAM(NIN_SELECT, 1));
        bool d1 = app.GetDesired();
        SendMessageW(app.hwnd(), WM_TRAYICON, 0, MAKELPARAM(NIN_SELECT, 1));
        bool d2 = app.GetDesired();
        add(L"toggle", d1 && !d2, L"");

        SendMessageW(app.hwnd(), WM_COMMAND, 1001, 0);   // animeradio (.m3u)
        app.StopNow();                                    // cancel pending resolution
        WaitFor([&]() { return false; }, 1500, app);
        bool cancelled = !app.GetDesired() && app.GetState() == App::Stopped && app.StreamHandle() == 0;
        add(L"cancel-m3u", cancelled, L"");

        SendMessageW(app.hwnd(), WM_COMMAND, 1001, 0);   // .m3u (older gen)
        SendMessageW(app.hwnd(), WM_COMMAND, 1003, 0);   // direct (newer gen) - latest must win
        WaitFor([&]() {
            return (app.GetState() == App::Playing && app.MediaHasAudio() && app.MediaTime() >= 1.0) ||
                   app.GetState() == App::Error;
        }, 35000, app);
        bool rapidOk = app.GetState() == App::Playing && app.MediaHasAudio() && app.MediaTime() >= 1.0 &&
                       app.GetSelected() == 3;
        add(L"rapid-latest-wins", rapidOk, L"sel=" + std::to_wstring(app.GetSelected()));
        app.StopNow();
        app.PumpOnce();

        for (int i = 0; i < kStationCount; ++i) {
            SendMessageW(app.hwnd(), WM_COMMAND, (WPARAM)(1000 + i), 0);
            WaitFor([&]() {
                return (app.GetState() == App::Playing && app.MediaHasAudio() && app.MediaTime() >= 1.0) ||
                       app.GetState() == App::Error;
            }, 35000, app);
            bool ok = app.GetState() == App::Playing && app.MediaHasAudio() && app.MediaTime() >= 1.0;
            add(L"station_" + std::to_wstring(i) + L"_" + kStations[i].id, ok,
                std::wstring(L"state=") + (ok ? L"Playing" : L"failed"));
            app.StopNow();
            app.PumpOnce();
            bool released = app.StreamHandle() == 0;
            add(L"release_" + std::to_wstring(i), released, L"");
        }

        add(L"icons", app.PlayIcon() && app.StopIcon() && app.PlayIcon() != app.StopIcon(), L"");

        SendMessageW(app.hwnd(), WM_COMMAND, (WPARAM)(1000 + 19), 0);
        app.StopNow();
        persistedId = kStations[19].id;
        app.Shutdown();
    }

    {
        App app2;
        if (!app2.Create(sub, true)) {
            add(L"persist", false, L"create2 failed");
        } else {
            bool ok = kStations[app2.GetSelected()].id == persistedId &&
                      app2.GetState() == App::Stopped && !app2.GetDesired();
            add(L"persist", ok, std::wstring(L"restored=") + kStations[app2.GetSelected()].id);
            SendMessageW(app2.hwnd(), WM_COMMAND, ID_MENU_EXIT, 0);
            bool exitOk = !app2.HasTray() && app2.StreamHandle() == 0 && app2.hwnd() == nullptr;
            add(L"exit", exitOk, L"");
        }
    }

    bool all = true;
    for (auto& it : items) if (!it.pass) all = false;
    WriteReport(reportPath, items, all);
    DeleteKeyTree(HKEY_CURRENT_USER, sub);
    return all ? 0 : 1;
}

static int RunProbe(const std::wstring& url, const std::wstring& reportPath) {
    std::vector<TestResult> items;
    std::wstring sub = L"Software\\RadioTray\\probe_" + std::to_wstring(GetCurrentProcessId());
    bool ok = false;
    double t = 0.0;
    int st = 0;
    {
        App app;
        if (!app.Create(sub, true)) {
            items.push_back({ L"probe", false, L"create failed" });
            WriteReport(reportPath, items, false);
            DeleteKeyTree(HKEY_CURRENT_USER, sub);
            return 1;
        }
        app.StartUrlPublic(url);
        WaitFor([&]() {
            return (app.GetState() == App::Playing && app.MediaTime() >= 1.0) ||
                   app.GetState() == App::Error;
        }, 35000, app);
        t = app.MediaTime();
        st = (int)app.GetState();
        ok = app.GetState() == App::Playing && t >= 1.0;
        app.DoExit();
    }
    std::wstring detail = L"state=" + std::to_wstring(st) + L" time=" + std::to_wstring(t);
    items.push_back({ L"probe", ok, detail });
    WriteReport(reportPath, items, ok);
    DeleteKeyTree(HKEY_CURRENT_USER, sub);
    return ok ? 0 : 1;
}

// ---------- entry ----------
int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hrCo)) return 1;

    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    int rc = 1;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        if (argc >= 3 && wcscmp(argv[1], L"--self-test") == 0) {
            rc = RunSelfTest(argv[2]);
        } else if (argc >= 4 && wcscmp(argv[1], L"--probe-url") == 0) {
            rc = RunProbe(argv[2], argv[3]);
        } else {
            // Sequential ownership: fully resolve the new mutex first so a
            // second instance never creates the legacy guard before deciding
            // it has lost. Only the fresh owner of the new mutex proceeds to
            // create the legacy guard.
            HANDLE mutex = CreateMutexW(nullptr, FALSE, kMutexName);
            if (!mutex) {
                rc = 1;
            } else if (GetLastError() == ERROR_ALREADY_EXISTS) {
                // Another new binary is running.
                CloseHandle(mutex);
                rc = 0;
            } else {
                // Fresh owner of the new mutex; now guard legacy binaries.
                HANDLE mutexOld = CreateMutexW(nullptr, FALSE, kPrevMutexName);
                if (!mutexOld) {
                    CloseHandle(mutex);
                    rc = 1;
                } else if (GetLastError() == ERROR_ALREADY_EXISTS) {
                    // A legacy binary is already running.
                    CloseHandle(mutexOld);
                    CloseHandle(mutex);
                    rc = 0;
                } else {
                    App app;
                    if (app.Create(kSettingsKey, false)) {
                        // Keep the Run entry pointing at this copy if the EXE was moved.
                        if (IsAutostartEnabled()) SetAutostart(true);
                        app.Run();
                        rc = 0;
                    }
                    CloseHandle(mutexOld);
                    CloseHandle(mutex);
                }
            }
        }
        LocalFree(argv);
    }

    CoUninitialize();
    return rc;
}
