#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <new>

#include "winamp_dsp.h"

namespace {

constexpr int kPort = 6969;
constexpr int kBitrateKbps = 256;
constexpr int kOutputSampleRate = 48000;
constexpr int kMaxClients = 16;
constexpr size_t kMaxQueueBytes = 8u * 1024u * 1024u;
constexpr ULONGLONG kPrebufferMilliseconds = 750;
constexpr size_t kPrebufferTargetBytes =
    static_cast<size_t>(kBitrateKbps) * 1000u / 8u * kPrebufferMilliseconds / 1000u;
constexpr UINT_PTR kUiTimerId = 1;

HINSTANCE g_moduleInstance = nullptr;
HWND g_window = nullptr;
HWND g_statusLabel = nullptr;
HWND g_listenersLabel = nullptr;
HWND g_startButton = nullptr;
HWND g_stopButton = nullptr;
HFONT g_uiFont = nullptr;

CRITICAL_SECTION g_logLock;
bool g_logLockReady = false;
HANDLE g_logFile = INVALID_HANDLE_VALUE;
wchar_t g_logPath[MAX_PATH * 2] = {};

void LoggerWrite(const char *format, ...) {
    if (!g_logLockReady || g_logFile == INVALID_HANDLE_VALUE) {
        return;
    }

    char message[2048];
    va_list args;
    va_start(args, format);
    _vsnprintf_s(message, sizeof(message), _TRUNCATE, format, args);
    va_end(args);

    SYSTEMTIME now;
    GetLocalTime(&now);
    char line[2300];
    const int length = _snprintf_s(
        line,
        sizeof(line),
        _TRUNCATE,
        "%04u-%02u-%02u %02u:%02u:%02u.%03u [tid=%lu] %s\r\n",
        now.wYear,
        now.wMonth,
        now.wDay,
        now.wHour,
        now.wMinute,
        now.wSecond,
        now.wMilliseconds,
        static_cast<unsigned long>(GetCurrentThreadId()),
        message);

    if (length <= 0) {
        return;
    }

    EnterCriticalSection(&g_logLock);
    DWORD written = 0;
    SetFilePointer(g_logFile, 0, nullptr, FILE_END);
    WriteFile(g_logFile, line, static_cast<DWORD>(length), &written, nullptr);
    FlushFileBuffers(g_logFile);
    LeaveCriticalSection(&g_logLock);
}

void LoggerInitialize() {
    if (!g_logLockReady) {
        InitializeCriticalSection(&g_logLock);
        g_logLockReady = true;
    }
    if (g_logFile != INVALID_HANDLE_VALUE) {
        return;
    }

    wchar_t localAppData[MAX_PATH] = {};
    const DWORD count = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
    if (count == 0 || count >= MAX_PATH) {
        GetTempPathW(MAX_PATH, localAppData);
    }
    wcscpy_s(g_logPath, localAppData);
    const size_t pathLength = wcslen(g_logPath);
    if (pathLength > 0 && g_logPath[pathLength - 1] != L'\\') {
        wcscat_s(g_logPath, L"\\");
    }
    wcscat_s(g_logPath, L"AIMP-ETS2-Cast");
    CreateDirectoryW(g_logPath, nullptr);
    wcscat_s(g_logPath, L"\\ets2cast.log");

    g_logFile = CreateFileW(
        g_logPath,
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (g_logFile == INVALID_HANDLE_VALUE) {
        if (GetModuleFileNameW(g_moduleInstance, g_logPath, _countof(g_logPath))) {
            wchar_t *slash = wcsrchr(g_logPath, L'\\');
            if (slash) {
                slash[1] = L'\0';
                wcscat_s(g_logPath, L"ets2cast.log");
                g_logFile = CreateFileW(
                    g_logPath,
                    FILE_APPEND_DATA,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr,
                    OPEN_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL,
                    nullptr);
            }
        }
    }
}

void LoggerShutdown() {
    if (g_logFile != INVALID_HANDLE_VALUE) {
        CloseHandle(g_logFile);
        g_logFile = INVALID_HANDLE_VALUE;
    }
    if (g_logLockReady) {
        DeleteCriticalSection(&g_logLock);
        g_logLockReady = false;
    }
}

struct AudioBlock {
    AudioBlock *next;
    DWORD byteCount;
    int numSamples;
    int bitsPerSample;
    int channels;
    int sampleRate;
    unsigned char data[1];
};

struct EncodedBlock {
    EncodedBlock *next;
    DWORD byteCount;
    unsigned char data[1];
};

using lame_t = void *;
using LameInitProc = lame_t (__cdecl *)(void);
using LameCloseProc = int (__cdecl *)(lame_t);
using LameSetIntProc = int (__cdecl *)(lame_t, int);
using LameInitParamsProc = int (__cdecl *)(lame_t);
using LameEncodeInterleavedProc = int (__cdecl *)(lame_t, short *, int, unsigned char *, int);
using LameEncodeFlushProc = int (__cdecl *)(lame_t, unsigned char *, int);

struct LameApi {
    LameInitProc init;
    LameCloseProc close;
    LameSetIntProc setInSampleRate;
    LameSetIntProc setOutSampleRate;
    LameSetIntProc setNumChannels;
    LameSetIntProc setBitrate;
    LameSetIntProc setMode;
    LameSetIntProc setQuality;
    LameSetIntProc setWriteVbrTag;
    LameSetIntProc setDisableReservoir;
    LameInitParamsProc initParams;
    LameEncodeInterleavedProc encodeInterleaved;
    LameEncodeFlushProc encodeFlush;
};

enum CastState : LONG {
    kStateStopped = 0,
    kStateBroadcasting = 1,
    kStateError = 2,
};

class CastEngine {
public:
    explicit CastEngine(HWND parentWindow)
        : parentWindow_(parentWindow),
          state_(kStateStopped),
          running_(0),
          encoderReady_(0),
          listenSocket_(INVALID_SOCKET),
          serverThread_(nullptr),
          encoderThread_(nullptr),
          stopEvent_(nullptr),
          queueEvent_(nullptr),
          queueHead_(nullptr),
          queueTail_(nullptr),
          queueBytes_(0),
          droppedBlocks_(0),
          handshakeThreads_(0),
          clientCount_(0),
          prebufferHead_(nullptr),
          prebufferTail_(nullptr),
          prebufferBytes_(0),
          prebufferReady_(false),
          winsockReady_(false),
          lameModule_(nullptr),
          ownsLameModule_(false),
          lameHandle_(nullptr),
          encoderInputRate_(0),
          currentChannels_(0),
          currentBits_(0),
          lastInputTick_(0),
          broadcastStartTick_(0),
          firstPcmTick_(0),
          firstMp3FrameTick_(0) {
        ZeroMemory(&lame_, sizeof(lame_));
        ZeroMemory(errorText_, sizeof(errorText_));
        for (int i = 0; i < kMaxClients; ++i) {
            clients_[i] = INVALID_SOCKET;
        }
        InitializeCriticalSection(&queueLock_);
        InitializeCriticalSection(&clientsLock_);
        InitializeCriticalSection(&stateLock_);
        InitializeCriticalSection(&lifecycleLock_);
        stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        queueEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        LoggerWrite("engine created; architecture=%u-bit", static_cast<unsigned>(sizeof(void *) * 8));
    }

    ~CastEngine() {
        Stop();
        CloseEncoder(false);
        if (ownsLameModule_ && lameModule_) {
            FreeLibrary(lameModule_);
        }
        if (winsockReady_) {
            WSACleanup();
        }
        if (queueEvent_) {
            CloseHandle(queueEvent_);
        }
        if (stopEvent_) {
            CloseHandle(stopEvent_);
        }
        DeleteCriticalSection(&lifecycleLock_);
        DeleteCriticalSection(&stateLock_);
        DeleteCriticalSection(&clientsLock_);
        DeleteCriticalSection(&queueLock_);
        LoggerWrite("engine destroyed");
    }

    bool Start() {
        EnterCriticalSection(&lifecycleLock_);
        if (InterlockedCompareExchange(&running_, 0, 0) != 0) {
            LeaveCriticalSection(&lifecycleLock_);
            return true;
        }

        ClearError();
        if (!stopEvent_ || !queueEvent_) {
            SetError("Unable to create worker synchronization objects", static_cast<DWORD>(GetLastError()));
            LeaveCriticalSection(&lifecycleLock_);
            return false;
        }
        if (!LoadLame()) {
            LeaveCriticalSection(&lifecycleLock_);
            return false;
        }
        if (!winsockReady_) {
            WSADATA data;
            const int result = WSAStartup(MAKEWORD(2, 2), &data);
            if (result != 0) {
                SetError("WSAStartup failed", static_cast<DWORD>(result));
                LeaveCriticalSection(&lifecycleLock_);
                return false;
            }
            winsockReady_ = true;
        }

        SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == INVALID_SOCKET) {
            SetError("Unable to create HTTP listener socket", static_cast<DWORD>(WSAGetLastError()));
            LeaveCriticalSection(&lifecycleLock_);
            return false;
        }
        BOOL exclusive = TRUE;
        setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char *>(&exclusive), sizeof(exclusive));

        sockaddr_in address;
        ZeroMemory(&address, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_port = htons(kPort);
        address.sin_addr.s_addr = inet_addr("127.0.0.1");
        if (bind(listener, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == SOCKET_ERROR) {
            const DWORD code = static_cast<DWORD>(WSAGetLastError());
            closesocket(listener);
            if (code == WSAEADDRINUSE) {
                SetError("Port 6969 is already in use", code);
            } else {
                SetError("Unable to bind 127.0.0.1:6969", code);
            }
            LeaveCriticalSection(&lifecycleLock_);
            return false;
        }
        if (listen(listener, SOMAXCONN) == SOCKET_ERROR) {
            const DWORD code = static_cast<DWORD>(WSAGetLastError());
            closesocket(listener);
            SetError("Unable to listen on 127.0.0.1:6969", code);
            LeaveCriticalSection(&lifecycleLock_);
            return false;
        }

        u_long nonBlocking = 1;
        ioctlsocket(listener, FIONBIO, &nonBlocking);
        ClearPrebuffer();
        broadcastStartTick_ = GetTickCount64();
        firstPcmTick_ = 0;
        firstMp3FrameTick_ = 0;
        listenSocket_ = listener;
        ResetEvent(stopEvent_);
        ResetEvent(queueEvent_);
        InterlockedExchange(&encoderReady_, 0);
        InterlockedExchange(&running_, 1);

        serverThread_ = CreateThread(nullptr, 0, ServerThreadThunk, this, 0, nullptr);
        encoderThread_ = CreateThread(nullptr, 0, EncoderThreadThunk, this, 0, nullptr);
        if (!serverThread_ || !encoderThread_) {
            const DWORD code = GetLastError();
            InterlockedExchange(&running_, 0);
            SetEvent(stopEvent_);
            closesocket(listenSocket_);
            listenSocket_ = INVALID_SOCKET;
            if (serverThread_) {
                WaitForSingleObject(serverThread_, 3000);
                CloseHandle(serverThread_);
                serverThread_ = nullptr;
            }
            if (encoderThread_) {
                WaitForSingleObject(encoderThread_, 3000);
                CloseHandle(encoderThread_);
                encoderThread_ = nullptr;
            }
            SetError("Unable to create broadcast worker threads", code);
            LeaveCriticalSection(&lifecycleLock_);
            return false;
        }

        InterlockedExchange(&state_, kStateBroadcasting);
        LoggerWrite("HTTP server started; bind=127.0.0.1 port=%d endpoint=/stream", kPort);
        LoggerWrite("broadcast start requested; format=MP3 bitrate=%d kbps output_rate=%d", kBitrateKbps, kOutputSampleRate);
        LoggerWrite(
            "startup prebuffer reset; target_ms=%llu target_bytes=%llu",
            static_cast<unsigned long long>(kPrebufferMilliseconds),
            static_cast<unsigned long long>(kPrebufferTargetBytes));
        LeaveCriticalSection(&lifecycleLock_);
        return true;
    }

    void Stop() {
        EnterCriticalSection(&lifecycleLock_);
        const LONG wasRunning = InterlockedExchange(&running_, 0);
        if (!wasRunning && !serverThread_ && !encoderThread_) {
            if (InterlockedCompareExchange(&state_, 0, 0) != kStateError) {
                InterlockedExchange(&state_, kStateStopped);
            }
            LeaveCriticalSection(&lifecycleLock_);
            return;
        }

        SetEvent(stopEvent_);
        SetEvent(queueEvent_);
        if (listenSocket_ != INVALID_SOCKET) {
            shutdown(listenSocket_, SD_BOTH);
            closesocket(listenSocket_);
            listenSocket_ = INVALID_SOCKET;
        }
        if (serverThread_) {
            WaitForSingleObject(serverThread_, 4000);
            CloseHandle(serverThread_);
            serverThread_ = nullptr;
        }
        for (int waitCount = 0; waitCount < 300 && InterlockedCompareExchange(&handshakeThreads_, 0, 0) != 0; ++waitCount) {
            Sleep(10);
        }
        if (InterlockedCompareExchange(&handshakeThreads_, 0, 0) != 0) {
            LoggerWrite("HTTP handshake workers still active during stop; count=%ld", InterlockedCompareExchange(&handshakeThreads_, 0, 0));
        }
        if (encoderThread_) {
            WaitForSingleObject(encoderThread_, 4000);
            CloseHandle(encoderThread_);
            encoderThread_ = nullptr;
        }
        CloseAllClients();
        ClearPrebuffer();
        ClearQueue();
        CloseEncoder(false);
        encoderInputRate_ = 0;
        currentChannels_ = 0;
        currentBits_ = 0;
        InterlockedExchange(&encoderReady_, 0);
        InterlockedExchange(&state_, kStateStopped);
        LoggerWrite("broadcast stopped; resources released");
        LeaveCriticalSection(&lifecycleLock_);
    }

    void Submit(const void *samples, int numSamples, int bitsPerSample, int channels, int sampleRate) {
        if (!samples || numSamples <= 0 || InterlockedCompareExchange(&running_, 0, 0) == 0) {
            return;
        }
        if (channels <= 0 || channels > 32 || sampleRate < 4000 || sampleRate > 384000 || bitsPerSample <= 0 || bitsPerSample % 8 != 0) {
            LoggerWrite(
                "invalid PCM block ignored; samples=%d bits=%d channels=%d rate=%d",
                numSamples,
                bitsPerSample,
                channels,
                sampleRate);
            return;
        }

        const uint64_t byteCount64 = static_cast<uint64_t>(numSamples) * static_cast<uint64_t>(channels) * static_cast<uint64_t>(bitsPerSample / 8);
        if (byteCount64 == 0 || byteCount64 > 4u * 1024u * 1024u) {
            LoggerWrite("oversized PCM block ignored; bytes=%llu", static_cast<unsigned long long>(byteCount64));
            return;
        }

        const size_t allocationSize = sizeof(AudioBlock) - 1 + static_cast<size_t>(byteCount64);
        AudioBlock *block = static_cast<AudioBlock *>(HeapAlloc(GetProcessHeap(), 0, allocationSize));
        if (!block) {
            LoggerWrite("PCM queue allocation failed; bytes=%llu", static_cast<unsigned long long>(byteCount64));
            return;
        }
        block->next = nullptr;
        block->byteCount = static_cast<DWORD>(byteCount64);
        block->numSamples = numSamples;
        block->bitsPerSample = bitsPerSample;
        block->channels = channels;
        block->sampleRate = sampleRate;
        CopyMemory(block->data, samples, static_cast<SIZE_T>(byteCount64));

        if (!TryEnterCriticalSection(&queueLock_)) {
            HeapFree(GetProcessHeap(), 0, block);
            const LONG dropped = InterlockedIncrement(&droppedBlocks_);
            if ((dropped % 100) == 1) {
                LoggerWrite("PCM queue busy; dropped_blocks=%ld", dropped);
            }
            return;
        }
        while (queueHead_ && queueBytes_ + block->byteCount > kMaxQueueBytes) {
            AudioBlock *old = queueHead_;
            queueHead_ = old->next;
            if (!queueHead_) {
                queueTail_ = nullptr;
            }
            queueBytes_ -= old->byteCount;
            HeapFree(GetProcessHeap(), 0, old);
            InterlockedIncrement(&droppedBlocks_);
        }
        if (queueTail_) {
            queueTail_->next = block;
        } else {
            queueHead_ = block;
        }
        queueTail_ = block;
        queueBytes_ += block->byteCount;
        LeaveCriticalSection(&queueLock_);
        SetEvent(queueEvent_);
    }

    CastState State() const {
        return static_cast<CastState>(InterlockedCompareExchange(const_cast<volatile LONG *>(&state_), 0, 0));
    }

    bool IsRunning() const {
        return InterlockedCompareExchange(const_cast<volatile LONG *>(&running_), 0, 0) != 0;
    }

    bool IsEncoderReady() const {
        return InterlockedCompareExchange(const_cast<volatile LONG *>(&encoderReady_), 0, 0) != 0;
    }

    int ListenerCount() {
        EnterCriticalSection(&clientsLock_);
        const int result = clientCount_;
        LeaveCriticalSection(&clientsLock_);
        return result;
    }

    void ErrorText(wchar_t *target, size_t targetCount) {
        if (!target || targetCount == 0) {
            return;
        }
        char local[512];
        EnterCriticalSection(&stateLock_);
        strcpy_s(local, errorText_);
        LeaveCriticalSection(&stateLock_);
        MultiByteToWideChar(CP_UTF8, 0, local, -1, target, static_cast<int>(targetCount));
        target[targetCount - 1] = L'\0';
    }

private:
    static DWORD WINAPI ServerThreadThunk(void *parameter) {
        return static_cast<CastEngine *>(parameter)->ServerThread();
    }

    static DWORD WINAPI EncoderThreadThunk(void *parameter) {
        return static_cast<CastEngine *>(parameter)->EncoderThread();
    }

    struct ClientHandshakeContext {
        CastEngine *engine;
        SOCKET socketValue;
    };

    static DWORD WINAPI ClientHandshakeThreadThunk(void *parameter) {
        ClientHandshakeContext *context = static_cast<ClientHandshakeContext *>(parameter);
        CastEngine *engine = context->engine;
        const SOCKET socketValue = context->socketValue;
        HeapFree(GetProcessHeap(), 0, context);
        engine->ClientHandshake(socketValue);
        InterlockedDecrement(&engine->handshakeThreads_);
        return 0;
    }

    void ClearError() {
        EnterCriticalSection(&stateLock_);
        errorText_[0] = '\0';
        LeaveCriticalSection(&stateLock_);
    }

    void SetError(const char *message, DWORD code) {
        EnterCriticalSection(&stateLock_);
        if (code) {
            _snprintf_s(errorText_, sizeof(errorText_), _TRUNCATE, "%s (code %lu)", message, static_cast<unsigned long>(code));
        } else {
            strncpy_s(errorText_, message, _TRUNCATE);
        }
        LeaveCriticalSection(&stateLock_);
        InterlockedExchange(&state_, kStateError);
        LoggerWrite("ERROR: %s; code=%lu", message, static_cast<unsigned long>(code));
    }

    static bool ContainsValidMp3Frame(const unsigned char *data, int length) {
        if (!data || length < 4) {
            return false;
        }
        for (int index = 0; index + 3 < length; ++index) {
            if (data[index] != 0xff || (data[index + 1] & 0xe0) != 0xe0) {
                continue;
            }
            const int version = (data[index + 1] >> 3) & 0x03;
            const int layer = (data[index + 1] >> 1) & 0x03;
            const int bitrateIndex = (data[index + 2] >> 4) & 0x0f;
            const int sampleRateIndex = (data[index + 2] >> 2) & 0x03;
            if (version != 1 && layer != 0 && bitrateIndex != 0 && bitrateIndex != 15 && sampleRateIndex != 3) {
                return true;
            }
        }
        return false;
    }

    bool LoadLame() {
        if (lameModule_) {
            return true;
        }

        lameModule_ = GetModuleHandleW(L"libLAME.dll");
        if (!lameModule_) {
            wchar_t executablePath[MAX_PATH * 2] = {};
            if (GetModuleFileNameW(nullptr, executablePath, _countof(executablePath))) {
                wchar_t *slash = wcsrchr(executablePath, L'\\');
                if (slash) {
                    slash[1] = L'\0';
                    wcscat_s(executablePath, L"libLAME.dll");
                    lameModule_ = LoadLibraryW(executablePath);
                    ownsLameModule_ = lameModule_ != nullptr;
                }
            }
        }
        if (!lameModule_) {
            SetError("AIMP x64 libLAME.dll was not found", static_cast<DWORD>(GetLastError()));
            return false;
        }

#define LOAD_LAME(member, exportName) \
        lame_.member = reinterpret_cast<decltype(lame_.member)>(GetProcAddress(lameModule_, exportName)); \
        if (!lame_.member) { SetError("libLAME.dll is missing required export " exportName, static_cast<DWORD>(GetLastError())); return false; }

        LOAD_LAME(init, "lame_init");
        LOAD_LAME(close, "lame_close");
        LOAD_LAME(setInSampleRate, "lame_set_in_samplerate");
        LOAD_LAME(setOutSampleRate, "lame_set_out_samplerate");
        LOAD_LAME(setNumChannels, "lame_set_num_channels");
        LOAD_LAME(setBitrate, "lame_set_brate");
        LOAD_LAME(setMode, "lame_set_mode");
        LOAD_LAME(setQuality, "lame_set_quality");
        LOAD_LAME(setWriteVbrTag, "lame_set_bWriteVbrTag");
        LOAD_LAME(setDisableReservoir, "lame_set_disable_reservoir");
        LOAD_LAME(initParams, "lame_init_params");
        LOAD_LAME(encodeInterleaved, "lame_encode_buffer_interleaved");
        LOAD_LAME(encodeFlush, "lame_encode_flush");
#undef LOAD_LAME

        LoggerWrite("libLAME loaded from AIMP; encoder exports resolved");
        return true;
    }

    bool InitializeEncoder(int inputRate, int channels, int bitsPerSample) {
        if (bitsPerSample != 16) {
            SetError("Unsupported PCM depth from AIMP; this prototype requires 16-bit DSP PCM", static_cast<DWORD>(bitsPerSample));
            return false;
        }
        if (inputRate == encoderInputRate_ && lameHandle_) {
            if (channels != currentChannels_ || bitsPerSample != currentBits_) {
                LoggerWrite(
                    "PCM parameters changed without encoder restart; rate=%d channels=%d bits=%d",
                    inputRate,
                    channels,
                    bitsPerSample);
            }
            currentChannels_ = channels;
            currentBits_ = bitsPerSample;
            return true;
        }

        if (lameHandle_) {
            LoggerWrite(
                "PCM sample rate changed; restarting encoder only; old_rate=%d new_rate=%d endpoint_unchanged=1",
                encoderInputRate_,
                inputRate);
            CloseEncoder(true);
        }

        lame_t handle = lame_.init();
        if (!handle) {
            SetError("lame_init failed", 0);
            return false;
        }

        int result = 0;
        result |= lame_.setInSampleRate(handle, inputRate);
        result |= lame_.setOutSampleRate(handle, kOutputSampleRate);
        result |= lame_.setNumChannels(handle, 2);
        result |= lame_.setBitrate(handle, kBitrateKbps);
        result |= lame_.setMode(handle, 0);
        result |= lame_.setQuality(handle, 2);
        result |= lame_.setWriteVbrTag(handle, 0);
        result |= lame_.setDisableReservoir(handle, 1);
        if (result != 0 || lame_.initParams(handle) < 0) {
            lame_.close(handle);
            SetError("Unable to initialize LAME MP3 encoder", static_cast<DWORD>(result));
            return false;
        }

        lameHandle_ = handle;
        encoderInputRate_ = inputRate;
        currentChannels_ = channels;
        currentBits_ = bitsPerSample;
        InterlockedExchange(&encoderReady_, 1);
        if (State() == kStateError) {
            ClearError();
            InterlockedExchange(&state_, kStateBroadcasting);
        }
        LoggerWrite(
            "encoder started; input_rate=%d input_channels=%d input_bits=%d output_rate=%d output_channels=2 bitrate=%d",
            inputRate,
            channels,
            bitsPerSample,
            kOutputSampleRate,
            kBitrateKbps);
        return true;
    }

    void CloseEncoder(bool flush) {
        if (!lameHandle_) {
            return;
        }
        if (flush && lame_.encodeFlush) {
            unsigned char tail[16384];
            const int produced = lame_.encodeFlush(lameHandle_, tail, static_cast<int>(sizeof(tail)));
            if (produced > 0) {
                Broadcast(tail, produced);
            }
        }
        lame_.close(lameHandle_);
        lameHandle_ = nullptr;
        InterlockedExchange(&encoderReady_, 0);
        LoggerWrite("encoder stopped");
    }

    AudioBlock *Dequeue() {
        EnterCriticalSection(&queueLock_);
        AudioBlock *block = queueHead_;
        if (block) {
            queueHead_ = block->next;
            if (!queueHead_) {
                queueTail_ = nullptr;
            }
            queueBytes_ -= block->byteCount;
            if (queueHead_) {
                SetEvent(queueEvent_);
            }
        }
        LeaveCriticalSection(&queueLock_);
        return block;
    }

    void ClearQueue() {
        EnterCriticalSection(&queueLock_);
        AudioBlock *block = queueHead_;
        queueHead_ = nullptr;
        queueTail_ = nullptr;
        queueBytes_ = 0;
        LeaveCriticalSection(&queueLock_);
        while (block) {
            AudioBlock *next = block->next;
            HeapFree(GetProcessHeap(), 0, block);
            block = next;
        }
    }

    bool EncodePcm16(const short *source, int numSamples, int channels, int inputRate) {
        if (!InitializeEncoder(inputRate, channels, 16)) {
            return false;
        }

        short *stereo = const_cast<short *>(source);
        bool allocated = false;
        if (channels != 2) {
            const size_t sampleCount = static_cast<size_t>(numSamples) * 2u;
            stereo = static_cast<short *>(HeapAlloc(GetProcessHeap(), 0, sampleCount * sizeof(short)));
            if (!stereo) {
                SetError("Unable to allocate stereo conversion buffer", static_cast<DWORD>(GetLastError()));
                return false;
            }
            allocated = true;
            for (int frame = 0; frame < numSamples; ++frame) {
                if (channels == 1) {
                    stereo[frame * 2] = source[frame];
                    stereo[frame * 2 + 1] = source[frame];
                } else {
                    stereo[frame * 2] = source[frame * channels];
                    stereo[frame * 2 + 1] = source[frame * channels + 1];
                }
            }
        }

        size_t outputSize = static_cast<size_t>(numSamples) * 8u + 16384u;
        if (outputSize > 4u * 1024u * 1024u) {
            outputSize = 4u * 1024u * 1024u;
        }
        unsigned char *output = static_cast<unsigned char *>(HeapAlloc(GetProcessHeap(), 0, outputSize));
        if (!output) {
            if (allocated) {
                HeapFree(GetProcessHeap(), 0, stereo);
            }
            SetError("Unable to allocate MP3 output buffer", static_cast<DWORD>(GetLastError()));
            return false;
        }

        const int produced = lame_.encodeInterleaved(
            lameHandle_,
            stereo,
            numSamples,
            output,
            static_cast<int>(outputSize));
        if (allocated) {
            HeapFree(GetProcessHeap(), 0, stereo);
        }
        if (produced < 0) {
            HeapFree(GetProcessHeap(), 0, output);
            SetError("LAME encoding failed", static_cast<DWORD>(-produced));
            return false;
        }
        if (produced > 0) {
            if (firstMp3FrameTick_ == 0 && ContainsValidMp3Frame(output, produced)) {
                firstMp3FrameTick_ = GetTickCount64();
                LoggerWrite(
                    "startup timing; first_mp3_frame_ms_after_start=%llu first_mp3_block_bytes=%d",
                    static_cast<unsigned long long>(firstMp3FrameTick_ - broadcastStartTick_),
                    produced);
            }
            Broadcast(output, produced);
        }
        HeapFree(GetProcessHeap(), 0, output);
        return true;
    }

    DWORD EncoderThread() {
        LoggerWrite("encoder worker started");
        while (InterlockedCompareExchange(&running_, 0, 0) != 0) {
            HANDLE handles[2] = {stopEvent_, queueEvent_};
            const DWORD wait = WaitForMultipleObjects(2, handles, FALSE, 100);
            if (wait == WAIT_OBJECT_0) {
                break;
            }

            AudioBlock *block = Dequeue();
            if (block) {
                if (firstPcmTick_ == 0) {
                    firstPcmTick_ = GetTickCount64();
                    LoggerWrite(
                        "startup timing; first_pcm_ms_after_start=%llu",
                        static_cast<unsigned long long>(firstPcmTick_ - broadcastStartTick_));
                }
                const bool changed = block->sampleRate != encoderInputRate_ ||
                    block->channels != currentChannels_ ||
                    block->bitsPerSample != currentBits_;
                if (changed) {
                    LoggerWrite(
                        "PCM parameters; rate=%d channels=%d bits=%d samples_per_channel=%d bytes=%lu",
                        block->sampleRate,
                        block->channels,
                        block->bitsPerSample,
                        block->numSamples,
                        static_cast<unsigned long>(block->byteCount));
                }
                if (block->bitsPerSample == 16) {
                    EncodePcm16(
                        reinterpret_cast<const short *>(block->data),
                        block->numSamples,
                        block->channels,
                        block->sampleRate);
                } else {
                    SetError("Unsupported PCM depth from AIMP; this prototype requires 16-bit DSP PCM", static_cast<DWORD>(block->bitsPerSample));
                }
                lastInputTick_ = GetTickCount64();
                HeapFree(GetProcessHeap(), 0, block);
                continue;
            }

            if (wait == WAIT_TIMEOUT && lameHandle_ && lastInputTick_ != 0 && GetTickCount64() - lastInputTick_ >= 150) {
                int frames = encoderInputRate_ / 10;
                if (frames < 400) {
                    frames = 400;
                }
                if (frames > 38400) {
                    frames = 38400;
                }
                const size_t bytes = static_cast<size_t>(frames) * 2u * sizeof(short);
                short *silence = static_cast<short *>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes));
                if (silence) {
                    EncodePcm16(silence, frames, 2, encoderInputRate_);
                    HeapFree(GetProcessHeap(), 0, silence);
                }
                lastInputTick_ = GetTickCount64();
            }
        }

        CloseEncoder(true);
        LoggerWrite("encoder worker stopped");
        return 0;
    }

    static bool SendAll(SOCKET socketValue, const char *data, int length) {
        int offset = 0;
        while (offset < length) {
            const int sent = send(socketValue, data + offset, length - offset, 0);
            if (sent <= 0) {
                return false;
            }
            offset += sent;
        }
        return true;
    }

    DWORD ServerThread() {
        LoggerWrite("HTTP worker started");
        while (InterlockedCompareExchange(&running_, 0, 0) != 0) {
            SOCKET listener = listenSocket_;
            if (listener == INVALID_SOCKET) {
                break;
            }
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(listener, &readSet);
            timeval timeout;
            timeout.tv_sec = 0;
            timeout.tv_usec = 200000;
            const int selected = select(0, &readSet, nullptr, nullptr, &timeout);
            if (selected <= 0) {
                continue;
            }

            sockaddr_in remote;
            int remoteLength = sizeof(remote);
            SOCKET client = accept(listener, reinterpret_cast<sockaddr *>(&remote), &remoteLength);
            if (client == INVALID_SOCKET) {
                continue;
            }
            LoggerWrite("HTTP connection accepted; awaiting request");
            ClientHandshakeContext *context = static_cast<ClientHandshakeContext *>(
                HeapAlloc(GetProcessHeap(), 0, sizeof(ClientHandshakeContext)));
            if (!context) {
                closesocket(client);
                continue;
            }
            context->engine = this;
            context->socketValue = client;
            InterlockedIncrement(&handshakeThreads_);
            HANDLE thread = CreateThread(nullptr, 0, ClientHandshakeThreadThunk, context, 0, nullptr);
            if (!thread) {
                InterlockedDecrement(&handshakeThreads_);
                HeapFree(GetProcessHeap(), 0, context);
                closesocket(client);
            }
            if (thread) {
                CloseHandle(thread);
            }
        }
        LoggerWrite("HTTP worker stopped");
        return 0;
    }

    void ClientHandshake(SOCKET client) {
        int receiveTimeout = 2000;
        int sendTimeout = 2000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&receiveTimeout), sizeof(receiveTimeout));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&sendTimeout), sizeof(sendTimeout));

        char request[4096];
        int used = 0;
        while (used < static_cast<int>(sizeof(request) - 1) && InterlockedCompareExchange(&running_, 0, 0) != 0) {
            const int received = recv(client, request + used, static_cast<int>(sizeof(request) - 1) - used, 0);
            if (received <= 0) {
                LoggerWrite("HTTP handshake ended before request; winsock_error=%d", WSAGetLastError());
                closesocket(client);
                return;
            }
            used += received;
            request[used] = '\0';
            if (strstr(request, "\r\n\r\n")) {
                break;
            }
        }
        request[used] = '\0';
        const bool validPath = strncmp(request, "GET /stream ", 12) == 0 || strncmp(request, "GET /stream?", 12) == 0;
        if (!validPath) {
            const char notFound[] =
                "HTTP/1.0 404 Not Found\r\n"
                "Content-Type: text/plain; charset=utf-8\r\n"
                "Content-Length: 9\r\n"
                "Connection: close\r\n\r\n"
                "Not Found";
            SendAll(client, notFound, static_cast<int>(sizeof(notFound) - 1));
            closesocket(client);
            LoggerWrite("HTTP request rejected; endpoint is /stream");
            return;
        }

        const char response[] =
            "HTTP/1.0 200 OK\r\n"
            "Content-Type: audio/mpeg\r\n"
            "Cache-Control: no-cache, no-store\r\n"
            "Pragma: no-cache\r\n"
            "Connection: close\r\n"
            "icy-name: AIMP Local\r\n"
            "icy-description: AIMP ETS2 Cast\r\n"
            "icy-br: 256\r\n"
            "icy-genre: Local\r\n\r\n";
        if (!SendAll(client, response, static_cast<int>(sizeof(response) - 1))) {
            LoggerWrite("HTTP response send failed; winsock_error=%d", WSAGetLastError());
            closesocket(client);
            return;
        }

        if (!AddClient(client)) {
            closesocket(client);
            LoggerWrite("listener not registered; connection closed; listener_limit=%d", kMaxClients);
        }
    }

    void ClearPrebufferLocked() {
        EncodedBlock *block = prebufferHead_;
        prebufferHead_ = nullptr;
        prebufferTail_ = nullptr;
        prebufferBytes_ = 0;
        prebufferReady_ = false;
        while (block) {
            EncodedBlock *next = block->next;
            HeapFree(GetProcessHeap(), 0, block);
            block = next;
        }
    }

    void ClearPrebuffer() {
        EnterCriticalSection(&clientsLock_);
        ClearPrebufferLocked();
        LeaveCriticalSection(&clientsLock_);
    }

    bool AppendPrebufferLocked(const unsigned char *data, int length) {
        const size_t allocationSize = sizeof(EncodedBlock) - 1u + static_cast<size_t>(length);
        EncodedBlock *block = static_cast<EncodedBlock *>(HeapAlloc(GetProcessHeap(), 0, allocationSize));
        if (!block) {
            LoggerWrite("MP3 prebuffer allocation failed; bytes=%d", length);
            return false;
        }
        block->next = nullptr;
        block->byteCount = static_cast<DWORD>(length);
        CopyMemory(block->data, data, static_cast<SIZE_T>(length));
        if (prebufferTail_) {
            prebufferTail_->next = block;
        } else {
            prebufferHead_ = block;
        }
        prebufferTail_ = block;
        prebufferBytes_ += static_cast<size_t>(length);

        // Drop only complete encoder output blocks so a new listener starts at
        // the boundary LAME supplied, never at an arbitrary byte in the ring.
        while (prebufferHead_ &&
               prebufferBytes_ > kPrebufferTargetBytes &&
               prebufferBytes_ - prebufferHead_->byteCount >= kPrebufferTargetBytes) {
            EncodedBlock *oldest = prebufferHead_;
            prebufferHead_ = oldest->next;
            if (!prebufferHead_) {
                prebufferTail_ = nullptr;
            }
            prebufferBytes_ -= oldest->byteCount;
            HeapFree(GetProcessHeap(), 0, oldest);
        }
        return true;
    }

    void SendToClientsLocked(const unsigned char *data, int length) {
        int index = 0;
        while (index < clientCount_) {
            const int sent = send(clients_[index], reinterpret_cast<const char *>(data), length, 0);
            if (sent != length) {
                shutdown(clients_[index], SD_BOTH);
                closesocket(clients_[index]);
                for (int move = index; move + 1 < clientCount_; ++move) {
                    clients_[move] = clients_[move + 1];
                }
                clients_[clientCount_ - 1] = INVALID_SOCKET;
                --clientCount_;
                LoggerWrite("listener disconnected; listeners=%d", clientCount_);
                continue;
            }
            ++index;
        }
    }

    bool AddClient(SOCKET client) {
        bool added = false;
        bool ready = false;
        size_t prebufferSent = 0;
        int failureCode = 0;
        EnterCriticalSection(&clientsLock_);
        ready = prebufferReady_;
        if (clientCount_ < kMaxClients) {
            bool sendReady = true;
            if (prebufferReady_) {
                for (EncodedBlock *block = prebufferHead_; block; block = block->next) {
                    if (!SendAll(client, reinterpret_cast<const char *>(block->data), static_cast<int>(block->byteCount))) {
                        failureCode = WSAGetLastError();
                        sendReady = false;
                        break;
                    }
                    prebufferSent += block->byteCount;
                }
            }
            u_long nonBlocking = 1;
            if (sendReady && ioctlsocket(client, FIONBIO, &nonBlocking) == 0) {
                clients_[clientCount_++] = client;
                added = true;
            } else if (sendReady) {
                failureCode = WSAGetLastError();
            }
        }
        const int count = clientCount_;
        LeaveCriticalSection(&clientsLock_);
        if (added) {
            LoggerWrite(
                "listener connected; listeners=%d prebuffer_ready=%d prebuffer_bytes_sent=%llu",
                count,
                ready ? 1 : 0,
                static_cast<unsigned long long>(prebufferSent));
        } else if (failureCode != 0) {
            LoggerWrite("listener prebuffer/send setup failed; winsock_error=%d", failureCode);
        }
        return added;
    }

    void Broadcast(const unsigned char *data, int length) {
        if (!data || length <= 0) {
            return;
        }
        EnterCriticalSection(&clientsLock_);
        const bool buffered = AppendPrebufferLocked(data, length);
        if (!prebufferReady_) {
            if (buffered && prebufferBytes_ >= kPrebufferTargetBytes) {
                prebufferReady_ = true;
                const ULONGLONG readyAt = GetTickCount64();
                LoggerWrite(
                    "startup prebuffer ready; ms_after_start=%llu bytes=%llu audio_ms=%llu listeners_waiting=%d",
                    static_cast<unsigned long long>(readyAt - broadcastStartTick_),
                    static_cast<unsigned long long>(prebufferBytes_),
                    static_cast<unsigned long long>(prebufferBytes_ * 8u / kBitrateKbps),
                    clientCount_);
                for (EncodedBlock *block = prebufferHead_; block; block = block->next) {
                    SendToClientsLocked(block->data, static_cast<int>(block->byteCount));
                }
            }
            LeaveCriticalSection(&clientsLock_);
            return;
        }
        SendToClientsLocked(data, length);
        LeaveCriticalSection(&clientsLock_);
    }

    void CloseAllClients() {
        EnterCriticalSection(&clientsLock_);
        for (int i = 0; i < clientCount_; ++i) {
            if (clients_[i] != INVALID_SOCKET) {
                shutdown(clients_[i], SD_BOTH);
                closesocket(clients_[i]);
                clients_[i] = INVALID_SOCKET;
            }
        }
        if (clientCount_ > 0) {
            LoggerWrite("all listeners disconnected; count=%d", clientCount_);
        }
        clientCount_ = 0;
        LeaveCriticalSection(&clientsLock_);
    }

    HWND parentWindow_;
    volatile LONG state_;
    volatile LONG running_;
    volatile LONG encoderReady_;
    SOCKET listenSocket_;
    HANDLE serverThread_;
    HANDLE encoderThread_;
    HANDLE stopEvent_;
    HANDLE queueEvent_;
    CRITICAL_SECTION queueLock_;
    CRITICAL_SECTION clientsLock_;
    CRITICAL_SECTION stateLock_;
    CRITICAL_SECTION lifecycleLock_;
    AudioBlock *queueHead_;
    AudioBlock *queueTail_;
    size_t queueBytes_;
    volatile LONG droppedBlocks_;
    volatile LONG handshakeThreads_;
    SOCKET clients_[kMaxClients];
    int clientCount_;
    EncodedBlock *prebufferHead_;
    EncodedBlock *prebufferTail_;
    size_t prebufferBytes_;
    bool prebufferReady_;
    bool winsockReady_;
    HMODULE lameModule_;
    bool ownsLameModule_;
    LameApi lame_;
    lame_t lameHandle_;
    int encoderInputRate_;
    int currentChannels_;
    int currentBits_;
    ULONGLONG lastInputTick_;
    ULONGLONG broadcastStartTick_;
    ULONGLONG firstPcmTick_;
    ULONGLONG firstMp3FrameTick_;
    char errorText_[512];
};

CastEngine *g_engine = nullptr;

void ApplyFont(HWND control) {
    if (control && g_uiFont) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
    }
}

HWND CreateText(HWND parent, int x, int y, int width, int height, const wchar_t *text) {
    HWND value = CreateWindowExW(
        0,
        L"STATIC",
        text,
        WS_CHILD | WS_VISIBLE,
        x,
        y,
        width,
        height,
        parent,
        nullptr,
        g_moduleInstance,
        nullptr);
    ApplyFont(value);
    return value;
}

void UpdateGui() {
    if (!g_window || !g_engine) {
        return;
    }

    wchar_t status[700] = {};
    if (g_engine->State() == kStateError) {
        wchar_t error[512] = {};
        g_engine->ErrorText(error, _countof(error));
        _snwprintf_s(status, _countof(status), _TRUNCATE, L"Status: Error - %s", error);
    } else if (g_engine->IsRunning() && g_engine->IsEncoderReady()) {
        wcscpy_s(status, L"Status: Broadcasting");
    } else if (g_engine->IsRunning()) {
        wcscpy_s(status, L"Status: Broadcasting - waiting for AIMP PCM");
    } else {
        wcscpy_s(status, L"Status: Stopped");
    }
    SetWindowTextW(g_statusLabel, status);

    wchar_t listeners[80];
    _snwprintf_s(listeners, _countof(listeners), _TRUNCATE, L"Listeners: %d", g_engine->ListenerCount());
    SetWindowTextW(g_listenersLabel, listeners);
    EnableWindow(g_startButton, !g_engine->IsRunning());
    EnableWindow(g_stopButton, g_engine->IsRunning() || g_engine->State() == kStateError);
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        NONCLIENTMETRICSW metrics;
        ZeroMemory(&metrics, sizeof(metrics));
        metrics.cbSize = sizeof(metrics);
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) {
            g_uiFont = CreateFontIndirectW(&metrics.lfMessageFont);
        }
        HWND title = CreateText(window, 22, 18, 450, 28, L"AIMP ETS2 Cast");
        if (g_uiFont) {
            LOGFONTW titleFontInfo = metrics.lfMessageFont;
            titleFontInfo.lfHeight = -20;
            titleFontInfo.lfWeight = FW_SEMIBOLD;
            HFONT titleFont = CreateFontIndirectW(&titleFontInfo);
            if (titleFont) {
                SendMessageW(title, WM_SETFONT, reinterpret_cast<WPARAM>(titleFont), TRUE);
                SetPropW(window, L"AIMPETS2CAST_TITLE_FONT", titleFont);
            }
        }
        g_statusLabel = CreateText(window, 22, 58, 470, 25, L"Status: Stopped");
        CreateText(window, 22, 92, 470, 24, L"URL: http://127.0.0.1:6969/stream");
        CreateText(window, 22, 122, 250, 24, L"Format: MP3");
        CreateText(window, 270, 122, 220, 24, L"Bitrate: 256 kbps");
        g_listenersLabel = CreateText(window, 22, 152, 250, 24, L"Listeners: 0");
        CreateText(window, 22, 181, 470, 36, L"The server is loopback-only and cannot be reached from other devices.");
        g_startButton = CreateWindowExW(
            0,
            L"BUTTON",
            L"Start",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            22,
            224,
            120,
            34,
            window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(1001)),
            g_moduleInstance,
            nullptr);
        g_stopButton = CreateWindowExW(
            0,
            L"BUTTON",
            L"Stop",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            155,
            224,
            120,
            34,
            window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(1002)),
            g_moduleInstance,
            nullptr);
        ApplyFont(g_startButton);
        ApplyFont(g_stopButton);
        SetTimer(window, kUiTimerId, 250, nullptr);
        UpdateGui();
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == 1001 && g_engine) {
            g_engine->Start();
            UpdateGui();
            return 0;
        }
        if (LOWORD(wParam) == 1002 && g_engine) {
            g_engine->Stop();
            UpdateGui();
            return 0;
        }
        break;
    case WM_TIMER:
        if (wParam == kUiTimerId) {
            UpdateGui();
            return 0;
        }
        break;
    case WM_CLOSE:
        ShowWindow(window, SW_HIDE);
        return 0;
    case WM_DESTROY: {
        KillTimer(window, kUiTimerId);
        HFONT titleFont = reinterpret_cast<HFONT>(RemovePropW(window, L"AIMPETS2CAST_TITLE_FONT"));
        if (titleFont) {
            DeleteObject(titleFont);
        }
        if (g_uiFont) {
            DeleteObject(g_uiFont);
            g_uiFont = nullptr;
        }
        g_window = nullptr;
        g_statusLabel = nullptr;
        g_listenersLabel = nullptr;
        g_startButton = nullptr;
        g_stopButton = nullptr;
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void ShowConfiguration(HWND parentWindow) {
    if (!g_window) {
        WNDCLASSEXW windowClass;
        ZeroMemory(&windowClass, sizeof(windowClass));
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = WindowProcedure;
        windowClass.hInstance = g_moduleInstance;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        windowClass.lpszClassName = L"AimpEts2CastWindow";
        RegisterClassExW(&windowClass);

        g_window = CreateWindowExW(
            WS_EX_TOOLWINDOW,
            windowClass.lpszClassName,
            L"AIMP ETS2 Cast",
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            530,
            310,
            parentWindow,
            nullptr,
            g_moduleInstance,
            nullptr);
    }
    if (g_window) {
        ShowWindow(g_window, SW_SHOWNORMAL);
        SetForegroundWindow(g_window);
        UpdateWindow(g_window);
    }
}

void DestroyConfiguration() {
    if (g_window) {
        DestroyWindow(g_window);
        g_window = nullptr;
    }
}

void __cdecl DspConfig(winampDSPModule *module) {
    ShowConfiguration(module ? module->parentWindow : nullptr);
}

int __cdecl DspInit(winampDSPModule *module) {
    LoggerInitialize();
    LoggerWrite("plugin init; name=AIMP ETS2 Cast version=0.1.1 architecture=%u-bit", static_cast<unsigned>(sizeof(void *) * 8));
    if (!module) {
        LoggerWrite("plugin init failed; null module");
        return 1;
    }
    if (!g_engine) {
        g_engine = new (std::nothrow) CastEngine(module->parentWindow);
    }
    module->userData = g_engine;
    if (!g_engine) {
        LoggerWrite("plugin init failed; engine allocation");
        return 1;
    }
    DspConfig(module);
    return 0;
}

int __cdecl DspModifySamples(
    winampDSPModule *module,
    short *samples,
    int numSamples,
    int bitsPerSample,
    int channels,
    int sampleRate) {
    CastEngine *engine = module ? static_cast<CastEngine *>(module->userData) : g_engine;
    if (engine) {
        engine->Submit(samples, numSamples, bitsPerSample, channels, sampleRate);
    }
    return numSamples;
}

void __cdecl DspQuit(winampDSPModule *module) {
    LoggerWrite("plugin quit requested");
    DestroyConfiguration();
    CastEngine *engine = module ? static_cast<CastEngine *>(module->userData) : g_engine;
    if (engine) {
        delete engine;
    }
    if (module) {
        module->userData = nullptr;
    }
    g_engine = nullptr;
    LoggerWrite("plugin unload complete");
    LoggerShutdown();
}

char g_pluginName[] = "AIMP ETS2 Cast";
winampDSPHeader g_header = {};
winampDSPModule g_dspModule = {};

winampDSPModule *__cdecl GetDspModule(int index) {
    if (index != 0) {
        return nullptr;
    }
    ZeroMemory(&g_dspModule, sizeof(g_dspModule));
    g_dspModule.description = g_pluginName;
    g_dspModule.libraryInstance = g_moduleInstance;
    g_dspModule.config = DspConfig;
    g_dspModule.init = DspInit;
    g_dspModule.modifySamples = DspModifySamples;
    g_dspModule.quit = DspQuit;
    return &g_dspModule;
}

}  // namespace

extern "C" __declspec(dllexport) winampDSPHeader *__cdecl winampDSPGetHeader2(void) {
    g_header.version = WINAMP_DSP_HDRVER;
    g_header.description = g_pluginName;
    g_header.getModule = GetDspModule;
    return &g_header;
}

extern "C" __declspec(dllexport) int __cdecl ets2cast_test_start(void) {
    return g_engine && g_engine->Start() ? 1 : 0;
}

extern "C" __declspec(dllexport) void __cdecl ets2cast_test_stop(void) {
    if (g_engine) {
        g_engine->Stop();
    }
}

extern "C" __declspec(dllexport) int __cdecl ets2cast_test_status(void) {
    if (!g_engine) {
        return -1;
    }
    return static_cast<int>(g_engine->State());
}

extern "C" __declspec(dllexport) const wchar_t *__cdecl ets2cast_test_log_path(void) {
    return g_logPath;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_moduleInstance = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
