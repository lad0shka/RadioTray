#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#define _UNICODE

#include "stream_metadata.h"

#include <windows.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace streammeta {

namespace {

const size_t kMaxTagBytes    = 65536;
const size_t kMaxTagEntries  = 256;
const size_t kTooltipMaxUnits = 127; // NOTIFYICONDATAW.szTip is 128 (incl. NUL)

// RAII wrapper for BASS_ChannelGetTags. The reference is always taken via
// BASS_TAG_INCREF and released deterministically with BASS_ChannelRef(FALSE);
// the caller must copy the returned pointer before the wrapper dies.
class ChannelTagRef {
public:
    ChannelTagRef(HSTREAM stream, DWORD tag) : m_stream(stream) {
        m_ptr = BASS_ChannelGetTags(stream, tag | BASS_TAG_INCREF);
        m_incremented = (m_ptr != nullptr);
    }
    ~ChannelTagRef() {
        if (m_incremented && m_stream)
            BASS_ChannelRef(m_stream, FALSE);
    }
    ChannelTagRef(const ChannelTagRef&) = delete;
    ChannelTagRef& operator=(const ChannelTagRef&) = delete;
    const char* Get() const { return m_ptr; }

private:
    HSTREAM m_stream = 0;
    const char* m_ptr = nullptr;
    bool    m_incremented = false;
};

size_t BoundedStrLen(const char* raw, size_t limit) {
    size_t n = 0;
    while (n < limit && raw[n] != '\0') ++n;
    return n;
}

bool AsciiEqualsIgnoreCase(std::string_view a, const char* b) {
    for (size_t i = 0; i < a.size(); ++i) {
        char cb = b[i];
        if (cb == '\0') return false;
        char ca = a[i];
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 'a' + 'A');
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 'a' + 'A');
        if (ca != cb) return false;
    }
    return b[a.size()] == '\0';
}

// Trim, collapse runs of whitespace to a single space, and strip control
// characters / line breaks so external metadata cannot inject tooltip lines.
std::wstring CleanText(const std::wstring& in) {
    std::wstring out;
    out.reserve(in.size());
    bool pendingSpace = false;
    for (wchar_t c : in) {
        if (c == L'\r' || c == L'\n' || c == L'\t' || c < 0x20 || c == 0x7F || c == L' ') {
            if (!out.empty()) pendingSpace = true;
            continue;
        }
        if (pendingSpace) {
            out += L' ';
            pendingSpace = false;
        }
        out += c;
    }
    return out;
}

std::wstring DecodeTagValue(std::string_view bytes) {
    if (bytes.empty()) return L"";
    std::wstring w;
    bool ok = false;
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                bytes.data(), (int)bytes.size(), nullptr, 0);
    if (n > 0) {
        w.resize((size_t)n);
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                bytes.data(), (int)bytes.size(), &w[0], n) == n) {
            ok = true;
        }
    }
    if (!ok) {
        n = MultiByteToWideChar(CP_ACP, 0, bytes.data(), (int)bytes.size(), nullptr, 0);
        if (n <= 0) return L"";
        w.assign((size_t)n, L'\0');
        if (MultiByteToWideChar(CP_ACP, 0, bytes.data(), (int)bytes.size(), &w[0], n) != n)
            return L"";
    }
    return CleanText(w);
}

// OGG tags are a bounded list of "NAME=value" UTF-8 strings, terminated by
// an empty entry. Only used as a fallback for streams without ICY META.
std::wstring ParseOggTags(const char* raw) {
    if (!raw) return L"";
    std::wstring artist;
    std::wstring title;
    size_t off = 0;
    for (size_t entry = 0; entry < kMaxTagEntries && off < kMaxTagBytes; ++entry) {
        size_t len = 0;
        while (off + len < kMaxTagBytes && raw[off + len] != '\0') ++len;
        if (len == 0) break;                         // empty entry terminates list
        if (off + len >= kMaxTagBytes) break;        // unterminated / oversized
        std::string_view sv(raw + off, len);
        off += len + 1;
        size_t eq = sv.find('=');
        if (eq == std::string_view::npos) continue;
        std::string_view key = sv.substr(0, eq);
        std::string_view val = sv.substr(eq + 1);
        if (AsciiEqualsIgnoreCase(key, "TITLE"))
            title = DecodeTagValue(val);
        else if (AsciiEqualsIgnoreCase(key, "ARTIST"))
            artist = DecodeTagValue(val);
    }
    if (title.empty()) return L"";                    // never show artist alone
    if (artist.empty()) return title;
    return artist + L" - " + title;
}

std::wstring TruncateWithEllipsis(const std::wstring& s, size_t maxUnits) {
    if (maxUnits == 0) return L"";
    if (s.size() <= maxUnits) return s;
    size_t cut = maxUnits - 1;                        // reserve one unit for ellipsis
    // Do not split a UTF-16 surrogate pair at the cut point.
    while (cut > 0 && cut < s.size()) {
        wchar_t prev = s[cut - 1];
        wchar_t next = s[cut];
        bool prevHigh = prev >= 0xD800 && prev <= 0xDBFF;
        bool nextLow  = next >= 0xDC00 && next <= 0xDFFF;
        if (prevHigh && nextLow) --cut;
        else break;
    }
    std::wstring out = s.substr(0, cut);
    out += L'\u2026';
    return out;
}

} // namespace

std::wstring ParseIcyTitle(const char* raw) {
    if (!raw) return L"";
    size_t rawLen = BoundedStrLen(raw, kMaxTagBytes);
    std::string_view buf(raw, rawLen);
    // Match "StreamTitle=" only at a field boundary (start or after ';'),
    // allowing optional whitespace, so occurrences inside e.g. StreamUrl
    // are ignored.
    static const std::string_view kKey = "StreamTitle=";
    size_t pos = 0;
    while (pos < buf.size()) {
        while (pos < buf.size() && (buf[pos] == ' ' || buf[pos] == '\t')) ++pos;
        if (pos >= buf.size()) break;
        if (buf.size() - pos >= kKey.size() &&
            AsciiEqualsIgnoreCase(buf.substr(pos, kKey.size()), kKey.data())) {
            size_t valStart = pos + kKey.size();
            if (valStart >= buf.size()) return L"";
            // Both single- and double-quoted values are accepted; the value
            // ends at the matching quote immediately followed by ';'.
            // Apostrophes / quotes not followed by ';' are preserved.
            char quote = buf[valStart];
            if (quote != '\'' && quote != '"') return L""; // non-quoted = invalid
            ++valStart;
            size_t valEnd = std::string_view::npos;
            for (size_t i = valStart; i + 1 < buf.size(); ++i) {
                if (buf[i] == quote && buf[i + 1] == ';') { valEnd = i; break; }
            }
            if (valEnd == std::string_view::npos) return L""; // unterminated
            return DecodeTagValue(buf.substr(valStart, valEnd - valStart));
        }
        size_t semi = buf.find(';', pos);
        if (semi == std::string_view::npos) break;
        pos = semi + 1;
    }
    return L"";
}

std::wstring ReadStreamTitle(HSTREAM stream) {
    if (!stream) return L"";
    {
        ChannelTagRef ref(stream, BASS_TAG_META);
        if (ref.Get()) {
            std::wstring t = ParseIcyTitle(ref.Get());
            if (!t.empty()) return t;
        }
    }
    {
        ChannelTagRef ref(stream, BASS_TAG_OGG);
        if (ref.Get()) {
            std::wstring t = ParseOggTags(ref.Get());
            if (!t.empty()) return t;
        }
    }
    return L"";
}

std::wstring BuildTrayTooltip(const std::wstring& station, const std::wstring& title) {
    if (title.empty())
        return TruncateWithEllipsis(station, kTooltipMaxUnits);
    // Keep the station line intact; only fall back to a truncated station when
    // the station itself would not even leave room for the separator.
    if (station.size() + 1 >= kTooltipMaxUnits)
        return TruncateWithEllipsis(station, kTooltipMaxUnits);
    std::wstring prefix = station;
    prefix += L'\n';
    size_t budget = kTooltipMaxUnits - prefix.size();
    return prefix + TruncateWithEllipsis(title, budget);
}

} // namespace streammeta
