#pragma once

#include <windows.h>

// Extracts the embedded bass.dll (RCDATA 101) to a per-user cache keyed by
// SHA-256 of the resource bytes, verifies the cached bytes byte-for-byte, and
// loads BASS by absolute path with LOAD_LIBRARY_SEARCH_SYSTEM32. Safe to call
// repeatedly: success is sticky, while failures are retried on later calls.
// Keeps the verified read handle and the HMODULE for the process lifetime so
// the file cannot be modified after verification. Returns false on failure.
bool EnsureEmbeddedBass();
