#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>

#include "winamp_dsp.h"

using TestStartProc = int (__cdecl *)(void);
using TestStopProc = void (__cdecl *)(void);
using TestLogPathProc = const wchar_t *(__cdecl *)(void);

int wmain(int argc, wchar_t **argv) {
    if (argc < 2) {
        fwprintf(stderr, L"Usage: dsp_host_smoke.exe <plugin.dll> [seconds] [libLAME.dll]\n");
        return 2;
    }
    const int seconds = argc >= 3 ? _wtoi(argv[2]) : 18;
    HMODULE lame = nullptr;
    if (argc >= 4) {
        lame = LoadLibraryW(argv[3]);
        if (!lame) {
            fwprintf(stderr, L"Unable to preload libLAME.dll: %lu\n", GetLastError());
            return 3;
        }
    }
    HMODULE plugin = LoadLibraryW(argv[1]);
    if (!plugin) {
        fwprintf(stderr, L"LoadLibrary failed: %lu\n", GetLastError());
        if (lame) FreeLibrary(lame);
        return 4;
    }
    auto getHeader = reinterpret_cast<WinampDSPGetHeader2Proc>(GetProcAddress(plugin, "winampDSPGetHeader2"));
    auto testStart = reinterpret_cast<TestStartProc>(GetProcAddress(plugin, "ets2cast_test_start"));
    auto testStop = reinterpret_cast<TestStopProc>(GetProcAddress(plugin, "ets2cast_test_stop"));
    auto testLogPath = reinterpret_cast<TestLogPathProc>(GetProcAddress(plugin, "ets2cast_test_log_path"));
    if (!getHeader || !testStart || !testStop) {
        fprintf(stderr, "Required export missing\n");
        FreeLibrary(plugin);
        if (lame) FreeLibrary(lame);
        return 5;
    }
    winampDSPHeader *header = getHeader();
    if (!header || header->version != WINAMP_DSP_HDRVER || !header->getModule) {
        fprintf(stderr, "Invalid DSP header\n");
        FreeLibrary(plugin);
        if (lame) FreeLibrary(lame);
        return 6;
    }
    winampDSPModule *module = header->getModule(0);
    if (!module || !module->init || !module->modifySamples || !module->quit) {
        fprintf(stderr, "Invalid DSP module\n");
        FreeLibrary(plugin);
        if (lame) FreeLibrary(lame);
        return 7;
    }
    module->parentWindow = GetConsoleWindow();
    module->libraryInstance = plugin;
    if (module->init(module) != 0 || !testStart()) {
        fprintf(stderr, "Plugin init/start failed\n");
        module->quit(module);
        FreeLibrary(plugin);
        if (lame) FreeLibrary(lame);
        return 8;
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
            return 9;
        }
    }
    if (testLogPath) {
        fwprintf(stdout, L"LOG PATH: %ls\n", testLogPath());
    }

    const int framesPerBlock = 1152;
    short samples[framesPerBlock * 2];
    double phase = 0.0;
    printf("SMOKE HOST READY http://127.0.0.1:6969/stream\n");
    fflush(stdout);
    const ULONGLONG start = GetTickCount64();
    while (GetTickCount64() - start < static_cast<ULONGLONG>(seconds) * 1000u) {
        const ULONGLONG elapsed = GetTickCount64() - start;
        if (elapsed >= 9000 && elapsed < 11000) {
            Sleep(50);
            continue;
        }
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
        module->modifySamples(module, samples, framesPerBlock, 16, 2, sampleRate);
        Sleep(sampleRate == 44100 ? 26 : 24);
    }

    testStop();
    module->quit(module);
    FreeLibrary(plugin);
    if (lame) FreeLibrary(lame);
    printf("SMOKE HOST COMPLETE\n");
    return 0;
}
