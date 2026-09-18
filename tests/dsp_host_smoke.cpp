#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "aimp_remote_info.h"
#include "winamp_dsp.h"

using TestStartProc = int (__cdecl *)(void);
using TestStopProc = void (__cdecl *)(void);
using TestLogPathProc = const wchar_t *(__cdecl *)(void);

static void WaitUntil(LONGLONG deadline) {
    for (;;) {
        LARGE_INTEGER now = {};
        QueryPerformanceCounter(&now);
        const LONGLONG remaining = deadline - now.QuadPart;
        if (remaining <= 0) {
            return;
        }
        // The smoke host deliberately busy-waits: the Windows timer in the
        // test environment rounds Sleep to a scheduler quantum and otherwise
        // feeds PCM substantially slower than realtime.
    }
}

static bool WriteRemoteInfo(
    unsigned char *mappingView,
    const wchar_t *artist,
    const wchar_t *title,
    const wchar_t *fileName) {
    if (!mappingView || !artist || !title || !fileName) {
        return false;
    }
    ZeroMemory(mappingView, ets2cast::kAimpRemoteInfoBytes);
    auto *info = reinterpret_cast<ets2cast::AimpRemoteFileInfoPrefix *>(mappingView);
    // AIMP's packed structure includes six native pointer fields. They are not
    // used across the mapping, but cbSizeOf includes them before the strings.
    info->cbSizeOf = static_cast<DWORD>(sizeof(*info) + 6u * sizeof(void *));
    info->active = TRUE;
    info->bitRate = 256;
    info->channels = 2;
    info->duration = 180000;
    info->fileSize = 12345678;
    info->sampleRate = 48000;
    info->trackId = 1;
    info->artistLength = static_cast<DWORD>(wcslen(artist));
    info->fileNameLength = static_cast<DWORD>(wcslen(fileName));
    info->titleLength = static_cast<DWORD>(wcslen(title));

    const size_t totalCharacters =
        static_cast<size_t>(info->artistLength) +
        static_cast<size_t>(info->fileNameLength) +
        static_cast<size_t>(info->titleLength);
    if (info->cbSizeOf + totalCharacters * sizeof(wchar_t) > ets2cast::kAimpRemoteInfoBytes) {
        return false;
    }
    unsigned char *cursor = mappingView + info->cbSizeOf;
    CopyMemory(cursor, artist, info->artistLength * sizeof(wchar_t));
    cursor += info->artistLength * sizeof(wchar_t);
    CopyMemory(cursor, fileName, info->fileNameLength * sizeof(wchar_t));
    cursor += info->fileNameLength * sizeof(wchar_t);
    CopyMemory(cursor, title, info->titleLength * sizeof(wchar_t));
    return true;
}

int wmain(int argc, wchar_t **argv) {
    if (argc < 2) {
        fwprintf(stderr, L"Usage: dsp_host_smoke.exe <plugin.dll> [seconds] [libLAME.dll]\n");
        return 2;
    }
    const int seconds = argc >= 3 ? _wtoi(argv[2]) : 18;

    wchar_t mappingName[128] = {};
    _snwprintf_s(
        mappingName,
        _countof(mappingName),
        _TRUNCATE,
        L"Local\\AIMP_ETS2_Cast_Test_RemoteInfo_%lu",
        static_cast<unsigned long>(GetCurrentProcessId()));
    HANDLE metadataMapping = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        0,
        static_cast<DWORD>(ets2cast::kAimpRemoteInfoBytes),
        mappingName);
    unsigned char *metadataView = metadataMapping
        ? static_cast<unsigned char *>(MapViewOfFile(
            metadataMapping,
            FILE_MAP_ALL_ACCESS,
            0,
            0,
            ets2cast::kAimpRemoteInfoBytes))
        : nullptr;
    if (!metadataMapping || !metadataView ||
        !WriteRemoteInfo(metadataView, L"Test Artist", L"First Track", L"C:\\Music\\first.flac") ||
        !SetEnvironmentVariableW(L"AIMP_ETS2_CAST_REMOTE_INFO_NAME", mappingName)) {
        fwprintf(stderr, L"Unable to prepare AIMP metadata mapping: %lu\n", GetLastError());
        if (metadataView) UnmapViewOfFile(metadataView);
        if (metadataMapping) CloseHandle(metadataMapping);
        return 3;
    }

    HMODULE lame = nullptr;
    if (argc >= 4) {
        lame = LoadLibraryW(argv[3]);
        if (!lame) {
            fwprintf(stderr, L"Unable to preload libLAME.dll: %lu\n", GetLastError());
            return 4;
        }
    }
    HMODULE plugin = LoadLibraryW(argv[1]);
    if (!plugin) {
        fwprintf(stderr, L"LoadLibrary failed: %lu\n", GetLastError());
        if (lame) FreeLibrary(lame);
        return 5;
    }
    auto getHeader = reinterpret_cast<WinampDSPGetHeader2Proc>(GetProcAddress(plugin, "winampDSPGetHeader2"));
    auto testStart = reinterpret_cast<TestStartProc>(GetProcAddress(plugin, "ets2cast_test_start"));
    auto testStop = reinterpret_cast<TestStopProc>(GetProcAddress(plugin, "ets2cast_test_stop"));
    auto testLogPath = reinterpret_cast<TestLogPathProc>(GetProcAddress(plugin, "ets2cast_test_log_path"));
    if (!getHeader || !testStart || !testStop) {
        fprintf(stderr, "Required export missing\n");
        FreeLibrary(plugin);
        if (lame) FreeLibrary(lame);
        return 6;
    }
    winampDSPHeader *header = getHeader();
    if (!header || header->version != WINAMP_DSP_HDRVER || !header->getModule) {
        fprintf(stderr, "Invalid DSP header\n");
        FreeLibrary(plugin);
        if (lame) FreeLibrary(lame);
        return 7;
    }
    winampDSPModule *module = header->getModule(0);
    if (!module || !module->init || !module->modifySamples || !module->quit) {
        fprintf(stderr, "Invalid DSP module\n");
        FreeLibrary(plugin);
        if (lame) FreeLibrary(lame);
        return 8;
    }
    module->parentWindow = GetConsoleWindow();
    module->libraryInstance = plugin;
    if (module->init(module) != 0 || !testStart()) {
        fprintf(stderr, "Plugin init/start failed\n");
        module->quit(module);
        FreeLibrary(plugin);
        if (lame) FreeLibrary(lame);
        return 9;
    }
    // Exercise repeated Start -> Stop -> Start cycles on the same loaded
    // plugin instance before feeding PCM.
    for (int cycle = 0; cycle < 3; ++cycle) {
        testStop();
        Sleep(100);
        if (!testStart()) {
            fprintf(stderr, "Plugin restart failed at cycle %d\n", cycle + 1);
            module->quit(module);
            FreeLibrary(plugin);
            if (lame) FreeLibrary(lame);
            return 10;
        }
    }
    if (testLogPath) {
        fwprintf(stdout, L"LOG PATH: %ls\n", testLogPath());
    }

    const int framesPerBlock = 1152;
    short samples[framesPerBlock * 2];
    double phase = 0.0;
    LARGE_INTEGER performanceFrequency = {};
    QueryPerformanceFrequency(&performanceFrequency);
    LONGLONG modifyCalls = 0;
    LONGLONG modifyTotalTicks = 0;
    LONGLONG modifyMaxTicks = 0;
    printf("SMOKE HOST READY http://127.0.0.1:6969/stream\n");
    fflush(stdout);
    const ULONGLONG start = GetTickCount64();
    const ULONGLONG secondMetadataAt = static_cast<ULONGLONG>(seconds) * 1000u / 3u;
    const ULONGLONG thirdMetadataAt = static_cast<ULONGLONG>(seconds) * 2000u / 3u;
    int metadataTrack = 1;
    while (GetTickCount64() - start < static_cast<ULONGLONG>(seconds) * 1000u) {
        const ULONGLONG elapsed = GetTickCount64() - start;
        if (elapsed >= secondMetadataAt && metadataTrack == 1) {
            WriteRemoteInfo(metadataView, L"Test Artist", L"Second Track", L"C:\\Music\\second.mp3");
            metadataTrack = 2;
            printf("METADATA_TRACK=2\n");
        } else if (elapsed >= thirdMetadataAt && metadataTrack == 2) {
            WriteRemoteInfo(metadataView, L"Test Artist", L"Third Track", L"C:\\Music\\third.ogg");
            metadataTrack = 3;
            printf("METADATA_TRACK=3\n");
        }
        if (elapsed >= 9000 && elapsed < 11000) {
            Sleep(50);
            continue;
        }
        LARGE_INTEGER blockCycleStarted = {};
        QueryPerformanceCounter(&blockCycleStarted);
        // Simulate a track transition from 44.1 kHz to 48 kHz while a
        // listener stays connected to the same HTTP endpoint.
        const int sampleRate = elapsed < 6000 ? 44100 : 48000;
        const double frequency = elapsed < 6000 ? 440.0 : (elapsed < 14000 ? 660.0 : 880.0);
        const double step = 2.0 * 3.14159265358979323846 * frequency / sampleRate;
        for (int frame = 0; frame < framesPerBlock; ++frame) {
            const short value = static_cast<short>(sin(phase) * 12000.0);
            samples[frame * 2] = value;
            samples[frame * 2 + 1] = value;
            phase += step;
            if (phase >= 2.0 * 3.14159265358979323846) {
                phase -= 2.0 * 3.14159265358979323846;
            }
        }
        LARGE_INTEGER modifyStarted = {};
        LARGE_INTEGER modifyFinished = {};
        QueryPerformanceCounter(&modifyStarted);
        module->modifySamples(module, samples, framesPerBlock, 16, 2, sampleRate);
        QueryPerformanceCounter(&modifyFinished);
        const LONGLONG modifyTicks = modifyFinished.QuadPart - modifyStarted.QuadPart;
        ++modifyCalls;
        modifyTotalTicks += modifyTicks;
        if (modifyTicks > modifyMaxTicks) {
            modifyMaxTicks = modifyTicks;
        }
        const LONGLONG blockDurationTicks =
            performanceFrequency.QuadPart * framesPerBlock / sampleRate;
        WaitUntil(blockCycleStarted.QuadPart + blockDurationTicks);
    }

    const double modifyAverageUs = modifyCalls > 0 && performanceFrequency.QuadPart > 0
        ? static_cast<double>(modifyTotalTicks) * 1000000.0 /
            static_cast<double>(performanceFrequency.QuadPart) / static_cast<double>(modifyCalls)
        : 0.0;
    const double modifyMaxUs = performanceFrequency.QuadPart > 0
        ? static_cast<double>(modifyMaxTicks) * 1000000.0 / static_cast<double>(performanceFrequency.QuadPart)
        : 0.0;
    printf("MODIFY_CALLS=%lld MODIFY_AVG_US=%.3f MODIFY_MAX_US=%.3f\n", modifyCalls, modifyAverageUs, modifyMaxUs);
    testStop();
    module->quit(module);
    FreeLibrary(plugin);
    if (lame) FreeLibrary(lame);
    SetEnvironmentVariableW(L"AIMP_ETS2_CAST_REMOTE_INFO_NAME", nullptr);
    UnmapViewOfFile(metadataView);
    CloseHandle(metadataMapping);
    printf("SMOKE HOST COMPLETE\n");
    return 0;
}
