#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stddef.h>

namespace ets2cast {

constexpr wchar_t kAimpRemoteInfoName[] = L"AIMP2_RemoteInfo";
constexpr size_t kAimpRemoteInfoBytes = 2048;

// AIMP serializes the six UTF-16 strings directly after cbSizeOf bytes.  The
// native pointer fields at the end of the SDK structure are deliberately not
// declared here: the prefix and cbSizeOf are sufficient and work across the
// x86/x64 structure-size difference.
#pragma pack(push, 1)
struct AimpRemoteFileInfoPrefix {
    DWORD cbSizeOf;
    BOOL active;
    DWORD bitRate;
    DWORD channels;
    DWORD duration;
    INT64 fileSize;
    DWORD rating;
    DWORD sampleRate;
    DWORD trackId;
    DWORD albumLength;
    DWORD artistLength;
    DWORD dateLength;
    DWORD fileNameLength;
    DWORD genreLength;
    DWORD titleLength;
};
#pragma pack(pop)

static_assert(sizeof(AimpRemoteFileInfoPrefix) == 64, "Unexpected AIMP RemoteInfo prefix layout");

}  // namespace ets2cast
