#include "embedded_bass.h"

#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <delayimp.h>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "delayimp.lib")

// Serializes EnsureEmbeddedBass() calls and the publication of g_module /
// g_cacheReadHandle. Success is sticky; failures are retried on later calls.
static std::mutex          g_mutex;
static bool                g_ok              = false;
static HMODULE             g_module          = nullptr;
// Verified read handle, held until process exit so cache cannot be rewritten
// between verification and module load. OS reclaims it at exit.
static HANDLE              g_cacheReadHandle = INVALID_HANDLE_VALUE;

namespace {
    // Releases a HANDLE on scope exit (including exception unwinding).
    struct HandleGuard {
        HANDLE h = INVALID_HANDLE_VALUE;
        ~HandleGuard() { reset(); }
        void reset() {
            if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); h = INVALID_HANDLE_VALUE; }
        }
    };

    // Owns a named mutex: releases ownership (if held) and closes the handle
    // on scope exit so a failed/excepted attempt never leaks ownership.
    struct NamedMutexGuard {
        HANDLE m     = nullptr;
        bool   owned = false;
        ~NamedMutexGuard() { release(); if (m) { CloseHandle(m); m = nullptr; } }
        void release() { if (owned && m) { ReleaseMutex(m); owned = false; } }
    };
}

static bool SHA256Hex(const BYTE* data, DWORD len, wchar_t out[65]) {
    BCRYPT_ALG_HANDLE  alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = false;
    DWORD cb = 0, hashLen = 0, objLen = 0;
    BYTE digest[32] = {};
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0) {
        if (BCryptGetProperty(alg, BCRYPT_HASH_LENGTH,   (PUCHAR)&hashLen, sizeof(hashLen), &cb, 0) == 0 && hashLen == 32 &&
            BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&objLen,  sizeof(objLen),  &cb, 0) == 0 && objLen  >  0) {
            std::vector<BYTE> obj(objLen);
            if (BCryptCreateHash(alg, &hash, obj.data(), objLen, nullptr, 0, 0) == 0) {
                if (BCryptHashData(hash, const_cast<PUCHAR>(data), len, 0) == 0 &&
                    BCryptFinishHash(hash, digest, 32, 0) == 0) {
                    ok = true;
                }
                BCryptDestroyHash(hash);
            }
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    if (!ok) return false;
    static const wchar_t* hex = L"0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
        out[i * 2]     = hex[(digest[i] >> 4) & 0x0F];
        out[i * 2 + 1] = hex[digest[i] & 0x0F];
    }
    out[64] = 0;
    return true;
}

// Opens path read-only with DENY-WRITE/DENY-DELETE, compares length and every
// byte. On success, keeps the handle open (prevents concurrent replacement)
// and returns it via *outHandle.
static bool FileMatches(const wchar_t* path, const BYTE* data, DWORD len, HANDLE* outHandle) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz = {};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart != (LONGLONG)len) { CloseHandle(h); return false; }
    std::vector<BYTE> buf(65536);
    DWORD total = 0;
    while (total < len) {
        DWORD want = len - total;
        if (want > (DWORD)buf.size()) want = (DWORD)buf.size();
        DWORD rd = 0;
        if (!ReadFile(h, buf.data(), want, &rd, nullptr) || rd != want) { CloseHandle(h); return false; }
        if (memcmp(buf.data(), data + total, rd) != 0) { CloseHandle(h); return false; }
        total += rd;
    }
    if (outHandle) *outHandle = h; else CloseHandle(h);
    return true;
}

static bool WriteFileExclusive(const wchar_t* path, const BYTE* data, DWORD len) {
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    DWORD total = 0;
    while (total < len) {
        DWORD wr = 0;
        if (!WriteFile(h, data + total, len - total, &wr, nullptr) || wr == 0) { ok = false; break; }
        total += wr;
    }
    if (ok) ok = (FlushFileBuffers(h) != 0);
    CloseHandle(h);
    return ok && total == len;
}

// Creates path if missing. If it already exists, it must actually be a
// directory (never accept an existing file as a "directory" collision).
static bool EnsureDirectory(const std::wstring& path) {
    if (CreateDirectoryW(path.c_str(), nullptr)) return true;
    if (GetLastError() != ERROR_ALREADY_EXISTS) return false;
    DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static bool DoEnsure() {
    HRSRC hrs = FindResourceW(nullptr, MAKEINTRESOURCEW(101), MAKEINTRESOURCEW(10));
    if (!hrs) return false;
    DWORD resLen = SizeofResource(nullptr, hrs);
    if (resLen == 0) return false;
    HGLOBAL hg = LoadResource(nullptr, hrs);
    if (!hg) return false;
    const BYTE* resData = (const BYTE*)LockResource(hg);
    if (!resData) return false;

    wchar_t hashHex[65] = {};
    if (!SHA256Hex(resData, resLen, hashHex)) return false;

    PWSTR localAppData = nullptr;
    if (SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &localAppData) != S_OK)
        return false;
    std::wstring base(localAppData);
    CoTaskMemFree(localAppData);

    // Use extended-length paths so long user profile names are handled
    // regardless of MAX_PATH. Adjust the UNC prefix correctly for shares.
    std::wstring ext;
    if (base.size() >= 2 && base[0] == L'\\' && base[1] == L'\\') {
        ext = L"\\\\?\\UNC\\" + base.substr(2);
    } else {
        ext = L"\\\\?\\" + base;
    }

    std::wstring dir1     = ext + L"\\RadioTray";
    std::wstring dir2     = dir1 + L"\\runtime";
    std::wstring dir      = dir2 + L"\\" + hashHex;
    std::wstring filePath = dir + L"\\bass.dll";

    // The parent (LocalAppData) is known to exist; create only our subtree,
    // verifying every existing path component is really a directory.
    if (!EnsureDirectory(dir1)) return false;
    if (!EnsureDirectory(dir2)) return false;
    if (!EnsureDirectory(dir))  return false;

    // Serialize cache creation per-content (named Local mutex by hash). The
    // guard releases the mutex and the handle even if verification throws.
    std::wstring mutexName = std::wstring(L"Local\\RadioTray.basscache.") + hashHex;
    NamedMutexGuard mtx;
    mtx.m = CreateMutexW(nullptr, FALSE, mutexName.c_str());
    if (!mtx.m) return false;
    DWORD wr = WaitForSingleObject(mtx.m, 30000);
    if (wr != WAIT_OBJECT_0 && wr != WAIT_ABANDONED) return false;
    mtx.owned = true;

    // Never trust the cached bytes: verify them byte-for-byte first. Only on
    // failure do we attempt a bounded rewrite + reverify, and only that path
    // sleeps. Permanent (e.g. read-only/locked) failures still fail cleanly.
    HandleGuard readGuard;
    bool verified = FileMatches(filePath.c_str(), resData, resLen, &readGuard.h);
    if (!verified) {
        readGuard.reset();
        const int kMaxAttempts = 4;
        for (int attempt = 0; attempt < kMaxAttempts && !verified; ++attempt) {
            bool wrote = WriteFileExclusive(filePath.c_str(), resData, resLen);
            if (wrote && FileMatches(filePath.c_str(), resData, resLen, &readGuard.h)) {
                verified = true;
                break;
            }
            readGuard.reset();
            if (attempt + 1 < kMaxAttempts) Sleep(25);
        }
    }
    if (!verified) return false;

    // Release the cross-process cache mutex once the bytes are stable. The
    // verified read handle stays open (deny-write/deny-delete) until process
    // exit so the file cannot be swapped between verification and load.
    mtx.release();

    HMODULE m = LoadLibraryExW(filePath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m) return false;

    // Publish: hand the verified read handle and module to process-lifetime
    // globals (still under g_mutex via EnsureEmbeddedBass()).
    g_cacheReadHandle = readGuard.h;
    readGuard.h       = INVALID_HANDLE_VALUE;
    g_module          = m;
    return true;
}

bool EnsureEmbeddedBass() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_ok) return true;
    try { g_ok = DoEnsure(); }
    catch (...) { g_ok = false; }
    return g_ok;
}

static FARPROC WINAPI BassDelayHook(unsigned dliNotify, PDelayLoadInfo pdli) {
    if (dliNotify == dliNotePreLoadLibrary && pdli && pdli->szDll &&
        lstrcmpiA(pdli->szDll, "bass.dll") == 0) {
        // Fail closed: never let the delay-load stub fall back to the default
        // name-search loader. The application gate normally prevents BASS
        // calls from reaching here without a loaded module; this exception
        // only fires if an unexpected caller bypasses that gate.
        if (!EnsureEmbeddedBass() || !g_module) {
            RaiseException(VcppException(ERROR_SEVERITY_ERROR, ERROR_MOD_NOT_FOUND),
                           EXCEPTION_NONCONTINUABLE, 0, nullptr);
        }
        return (FARPROC)g_module;
    }
    return nullptr;
}

// Recent Windows SDK declares __pfnDliNotifyHook2 as const PfnDliHook.
// Define DELAYIMP_INSECURE_WRITABLE_HOOKS if linking against an SDK that
// needs a non-const definition.
extern "C" const PfnDliHook __pfnDliNotifyHook2 = BassDelayHook;
