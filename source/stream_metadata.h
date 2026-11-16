#pragma once

#include <string>

#include "vendor/bass/bass.h"

namespace streammeta {

// Parse a single value from an ICY metadata block. Looks only for
// StreamTitle='...'; and returns empty when the key is missing or the
// block is malformed (unterminated). Apostrophes inside the title are
// preserved; the terminator is the exact quote+semicolon sequence.
std::wstring ParseIcyTitle(const char* raw);

// Read the current track title of a stream. Returns empty when none is
// available. Prefers ICY StreamTitle, falls back to OGG ARTIST/TITLE.
std::wstring ReadStreamTitle(HSTREAM stream);

// Build the tray tooltip EXACTLY as:
//   title empty:    <station>
//   title present:  <station>\n<artist - title>
// Result is capped to 127 UTF-16 code units and never splits a surrogate
// pair; truncation ends with a single U+2026 ellipsis.
std::wstring BuildTrayTooltip(const std::wstring& station, const std::wstring& title);

}
