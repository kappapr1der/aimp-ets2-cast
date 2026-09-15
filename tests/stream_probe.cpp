#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>

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

int main(int argc, char **argv) {
    const int seconds = argc >= 2 ? atoi(argv[1]) : 6;
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 2;
    }
    SOCKET socketValue = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(6969);
    address.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (socketValue == INVALID_SOCKET || connect(socketValue, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == SOCKET_ERROR) {
        fprintf(stderr, "connect failed: %d\n", WSAGetLastError());
        if (socketValue != INVALID_SOCKET) closesocket(socketValue);
        WSACleanup();
        return 3;
    }
    const char request[] = "GET /stream HTTP/1.0\r\nHost: 127.0.0.1:6969\r\nUser-Agent: AIMP-ETS2-Cast-Probe/1\r\n\r\n";
    if (!SendAll(socketValue, request, static_cast<int>(sizeof(request) - 1))) {
        fprintf(stderr, "request send failed\n");
        closesocket(socketValue);
        WSACleanup();
        return 4;
    }
    const ULONGLONG requestSentAt = GetTickCount64();

    unsigned char buffer[65536];
    int used = 0;
    bool headersDone = false;
    bool httpOk = false;
    bool contentTypeOk = false;
    int mp3SyncCount = 0;
    unsigned long long audioBytes = 0;
    ULONGLONG firstHeadersMs = ~0ull;
    ULONGLONG firstAudioMs = ~0ull;
    ULONGLONG firstMp3FrameMs = ~0ull;
    unsigned long long firstMp3FrameOffset = ~0ull;
    unsigned char firstAudioPrefix[4] = {};
    int firstAudioPrefixCount = 0;
    int startupChunkSizes[16] = {};
    ULONGLONG startupChunkTimes[16] = {};
    int startupChunkCount = 0;
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000u;
    while (GetTickCount64() < deadline && used < static_cast<int>(sizeof(buffer))) {
        const int received = recv(socketValue, reinterpret_cast<char *>(buffer + used), static_cast<int>(sizeof(buffer)) - used, 0);
        if (received <= 0) {
            break;
        }
        const ULONGLONG receivedAt = GetTickCount64();
        used += received;
        if (!headersDone) {
            for (int i = 0; i + 3 < used; ++i) {
                if (buffer[i] == '\r' && buffer[i + 1] == '\n' && buffer[i + 2] == '\r' && buffer[i + 3] == '\n') {
                    buffer[i] = '\0';
                    const char *headers = reinterpret_cast<const char *>(buffer);
                    httpOk = strstr(headers, "200 OK") != nullptr;
                    contentTypeOk = strstr(headers, "Content-Type: audio/mpeg") != nullptr;
                    firstHeadersMs = receivedAt - requestSentAt;
                    const int audioStart = i + 4;
                    if (used > audioStart) {
                        memmove(buffer, buffer + audioStart, used - audioStart);
                        used -= audioStart;
                    } else {
                        used = 0;
                    }
                    headersDone = true;
                    break;
                }
            }
        }
        if (headersDone && used > 4) {
            if (firstAudioMs == ~0ull) {
                firstAudioMs = receivedAt - requestSentAt;
            }
            if (firstAudioPrefixCount == 0) {
                firstAudioPrefixCount = used < 4 ? used : 4;
                memcpy(firstAudioPrefix, buffer, static_cast<size_t>(firstAudioPrefixCount));
            }
            if (startupChunkCount < static_cast<int>(sizeof(startupChunkSizes) / sizeof(startupChunkSizes[0]))) {
                startupChunkSizes[startupChunkCount] = used;
                startupChunkTimes[startupChunkCount] = receivedAt - requestSentAt;
                ++startupChunkCount;
            }
            for (int i = 0; i + 3 < used; ++i) {
                if (buffer[i] == 0xff && (buffer[i + 1] & 0xe0) == 0xe0 && (buffer[i + 1] & 0x18) != 0x08 && (buffer[i + 2] & 0xf0) != 0xf0) {
                    if (firstMp3FrameMs == ~0ull) {
                        firstMp3FrameMs = receivedAt - requestSentAt;
                        firstMp3FrameOffset = audioBytes + static_cast<unsigned long long>(i);
                    }
                    ++mp3SyncCount;
                }
            }
            audioBytes += static_cast<unsigned long long>(used);
            used = 0;
        }
    }
    shutdown(socketValue, SD_BOTH);
    closesocket(socketValue);
    WSACleanup();

    printf(
        "HTTP_OK=%d CONTENT_TYPE_OK=%d AUDIO_BYTES=%llu MP3_SYNC_CANDIDATES=%d "
        "HEADERS_MS=%llu FIRST_AUDIO_MS=%llu FIRST_MP3_FRAME_MS=%llu\n",
        httpOk ? 1 : 0,
        contentTypeOk ? 1 : 0,
        audioBytes,
        mp3SyncCount,
        firstHeadersMs,
        firstAudioMs,
        firstMp3FrameMs);
    printf(
        "FIRST_AUDIO_PREFIX=%02X%02X%02X%02X FIRST_MP3_FRAME_OFFSET=%llu\n",
        firstAudioPrefix[0],
        firstAudioPrefix[1],
        firstAudioPrefix[2],
        firstAudioPrefix[3],
        firstMp3FrameOffset);
    printf("STARTUP_CHUNKS=");
    for (int i = 0; i < startupChunkCount; ++i) {
        printf("%s%d@%llums", i == 0 ? "" : ",", startupChunkSizes[i], startupChunkTimes[i]);
    }
    printf("\n");
    if (!httpOk || !contentTypeOk || audioBytes < 16000 || mp3SyncCount < 5) {
        return 1;
    }
    printf("STREAM_PROBE_PASS\n");
    return 0;
}
