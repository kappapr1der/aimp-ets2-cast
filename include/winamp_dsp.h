#pragma once

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WINAMP_DSP_HDRVER 0x20

typedef struct winampDSPModule winampDSPModule;

typedef void (__cdecl *WinampDSPConfigProc)(winampDSPModule *module);
typedef int (__cdecl *WinampDSPInitProc)(winampDSPModule *module);
typedef int (__cdecl *WinampDSPModifySamplesProc)(
    winampDSPModule *module,
    short *samples,
    int numSamples,
    int bitsPerSample,
    int channels,
    int sampleRate);
typedef void (__cdecl *WinampDSPQuitProc)(winampDSPModule *module);

struct winampDSPModule {
    char *description;
    HWND parentWindow;
    HINSTANCE libraryInstance;
    WinampDSPConfigProc config;
    WinampDSPInitProc init;
    WinampDSPModifySamplesProc modifySamples;
    WinampDSPQuitProc quit;
    void *userData;
};

typedef struct winampDSPHeader {
    int version;
    char *description;
    winampDSPModule *(__cdecl *getModule)(int index);
} winampDSPHeader;

typedef winampDSPHeader *(__cdecl *WinampDSPGetHeader2Proc)(void);

#ifdef __cplusplus
}
#endif
