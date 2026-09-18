#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
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

static bool ReceiveExact(SOCKET socketValue, unsigned char *data, int length) {
    int offset = 0;
    while (offset < length) {
        const int received = recv(socketValue, reinterpret_cast<char *>(data + offset), length - offset, 0);
        if (received <= 0) {
            return false;
        }
        offset += received;
    }
    return true;
}

static int HeaderInteger(const char *headers, const char *name) {
    const size_t nameLength = strlen(name);
    const char *line = headers;
    while (line && *line) {
        const char *end = strstr(line, "\r\n");
        const size_t length = end ? static_cast<size_t>(end - line) : strlen(line);
        if (length > nameLength && _strnicmp(line, name, nameLength) == 0) {
            const char *value = line + nameLength;
            while (value < line + length && (*value == ' ' || *value == '\t')) {
                ++value;
            }
            return atoi(value);
        }
        line = end ? end + 2 : nullptr;
    }
    return 0;
}

static void ExtractStreamTitle(const unsigned char *metadata, int length, char *target, size_t targetCount) {
    if (!target || targetCount == 0) {
        return;
    }
    target[0] = '\0';
    if (!metadata || length <= 0) {
        return;
    }
    const char marker[] = "StreamTitle='";
    const char *text = reinterpret_cast<const char *>(metadata);
    const char *start = strstr(text, marker);
    if (!start) {
        return;
    }
    start += sizeof(marker) - 1;
    const char *end = strstr(start, "';");
    if (!end) {
        return;
    }
    size_t count = static_cast<size_t>(end - start);
    if (count >= targetCount) {
        count = targetCount - 1;
    }
    memcpy(target, start, count);
    target[count] = '\0';
}

int main(int argc, char **argv) {
    const int seconds = argc >= 2 ? atoi(argv[1]) : 12;
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

    int receiveTimeout = 3000;
    setsockopt(socketValue, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&receiveTimeout), sizeof(receiveTimeout));
    const char request[] =
        "GET /stream HTTP/1.0\r\n"
        "Host: 127.0.0.1:6969\r\n"
        "User-Agent: AIMP-ETS2-Cast-ICY-Probe/1\r\n"
        "Icy-MetaData: 1\r\n\r\n";
    if (!SendAll(socketValue, request, static_cast<int>(sizeof(request) - 1))) {
        fprintf(stderr, "request send failed\n");
        closesocket(socketValue);
        WSACleanup();
        return 4;
    }

    char headers[8192] = {};
    int headerBytes = 0;
    while (headerBytes < static_cast<int>(sizeof(headers) - 1) && !strstr(headers, "\r\n\r\n")) {
        const int received = recv(socketValue, headers + headerBytes, static_cast<int>(sizeof(headers) - 1) - headerBytes, 0);
        if (received <= 0) {
            fprintf(stderr, "header receive failed: %d\n", WSAGetLastError());
            closesocket(socketValue);
            WSACleanup();
            return 5;
        }
        headerBytes += received;
        headers[headerBytes] = '\0';
    }
    char *headerEnd = strstr(headers, "\r\n\r\n");
    const bool httpOk = strstr(headers, "200 OK") != nullptr;
    const bool contentTypeOk = strstr(headers, "Content-Type: audio/mpeg") != nullptr;
    const int metadataInterval = HeaderInteger(headers, "icy-metaint:");
    if (!headerEnd || !httpOk || !contentTypeOk || metadataInterval <= 0 || metadataInterval > 1024 * 1024) {
        fprintf(stderr, "invalid ICY response: HTTP=%d TYPE=%d METAINT=%d\n", httpOk ? 1 : 0, contentTypeOk ? 1 : 0, metadataInterval);
        closesocket(socketValue);
        WSACleanup();
        return 6;
    }

    const int bodyOffset = static_cast<int>((headerEnd + 4) - headers);
    int pendingOffset = bodyOffset;
    int pendingBytes = headerBytes - bodyOffset;
    unsigned long long audioBytes = 0;
    int mp3SyncCount = 0;
    int metadataBlocks = 0;
    int nonEmptyMetadataBlocks = 0;
    int distinctTitles = 0;
    char firstTitle[1024] = {};
    char lastTitle[1024] = {};
    unsigned char previousAudioByte = 0;
    bool havePreviousAudioByte = false;
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000u;

    auto receiveBytes = [&](unsigned char *target, int count) -> bool {
        int copied = 0;
        if (pendingBytes > 0) {
            const int take = pendingBytes < count ? pendingBytes : count;
            memcpy(target, headers + pendingOffset, static_cast<size_t>(take));
            pendingOffset += take;
            pendingBytes -= take;
            copied += take;
        }
        return copied == count || ReceiveExact(socketValue, target + copied, count - copied);
    };

    unsigned char *audio = static_cast<unsigned char *>(HeapAlloc(GetProcessHeap(), 0, static_cast<SIZE_T>(metadataInterval)));
    if (!audio) {
        closesocket(socketValue);
        WSACleanup();
        return 7;
    }
    while (GetTickCount64() < deadline) {
        if (!receiveBytes(audio, metadataInterval)) {
            break;
        }
        for (int index = 0; index < metadataInterval; ++index) {
            if (havePreviousAudioByte && previousAudioByte == 0xff && (audio[index] & 0xe0) == 0xe0) {
                ++mp3SyncCount;
            }
            previousAudioByte = audio[index];
            havePreviousAudioByte = true;
        }
        audioBytes += static_cast<unsigned long long>(metadataInterval);

        unsigned char units = 0;
        if (!receiveBytes(&units, 1)) {
            break;
        }
        const int metadataBytes = static_cast<int>(units) * 16;
        unsigned char metadata[4096 + 1] = {};
        if (metadataBytes > 4096 || (metadataBytes > 0 && !receiveBytes(metadata, metadataBytes))) {
            break;
        }
        ++metadataBlocks;
        if (metadataBytes > 0) {
            ++nonEmptyMetadataBlocks;
            char title[1024] = {};
            ExtractStreamTitle(metadata, metadataBytes, title, sizeof(title));
            if (title[0]) {
                if (!firstTitle[0]) {
                    strcpy_s(firstTitle, title);
                }
                if (strcmp(lastTitle, title) != 0) {
                    ++distinctTitles;
                }
                strcpy_s(lastTitle, title);
            }
        }
    }
    HeapFree(GetProcessHeap(), 0, audio);
    shutdown(socketValue, SD_BOTH);
    closesocket(socketValue);
    WSACleanup();

    printf(
        "HTTP_OK=%d CONTENT_TYPE_OK=%d ICY_METAINT=%d AUDIO_BYTES=%llu MP3_SYNC_CANDIDATES=%d "
        "METADATA_BLOCKS=%d NONEMPTY_METADATA_BLOCKS=%d DISTINCT_TITLES=%d\n",
        httpOk ? 1 : 0,
        contentTypeOk ? 1 : 0,
        metadataInterval,
        audioBytes,
        mp3SyncCount,
        metadataBlocks,
        nonEmptyMetadataBlocks,
        distinctTitles);
    printf("FIRST_TITLE=%s\nLAST_TITLE=%s\n", firstTitle, lastTitle);
    if (audioBytes < 16000 || mp3SyncCount < 5 || metadataBlocks < 1 ||
        nonEmptyMetadataBlocks < 1 || !firstTitle[0] || distinctTitles < 2 ||
        strcmp(firstTitle, lastTitle) == 0) {
        return 1;
    }
    printf("ICY_METADATA_PROBE_PASS\n");
    return 0;
}
