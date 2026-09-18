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

int main(int argc, char **argv) {
    const int stallMilliseconds = argc >= 2 ? atoi(argv[1]) : 20000;
    const int drainMilliseconds = argc >= 3 ? atoi(argv[2]) : 10000;
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 2;
    }

    SOCKET socketValue = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socketValue == INVALID_SOCKET) {
        WSACleanup();
        return 3;
    }
    int receiveBufferBytes = 4096;
    setsockopt(
        socketValue,
        SOL_SOCKET,
        SO_RCVBUF,
        reinterpret_cast<const char *>(&receiveBufferBytes),
        sizeof(receiveBufferBytes));
    int receiveTimeout = 3000;
    setsockopt(
        socketValue,
        SOL_SOCKET,
        SO_RCVTIMEO,
        reinterpret_cast<const char *>(&receiveTimeout),
        sizeof(receiveTimeout));

    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(6969);
    address.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(socketValue, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == SOCKET_ERROR) {
        fprintf(stderr, "connect failed: %d\n", WSAGetLastError());
        closesocket(socketValue);
        WSACleanup();
        return 4;
    }

    const char request[] =
        "GET /stream HTTP/1.0\r\n"
        "Host: 127.0.0.1:6969\r\n"
        "User-Agent: AIMP-ETS2-Cast-Backpressure-Probe/1\r\n"
        "Icy-MetaData: 1\r\n\r\n";
    if (!SendAll(socketValue, request, static_cast<int>(sizeof(request) - 1))) {
        fprintf(stderr, "request send failed\n");
        closesocket(socketValue);
        WSACleanup();
        return 5;
    }

    char headers[8192] = {};
    int headerBytes = 0;
    while (headerBytes < static_cast<int>(sizeof(headers) - 1) && !strstr(headers, "\r\n\r\n")) {
        const int received = recv(
            socketValue,
            headers + headerBytes,
            static_cast<int>(sizeof(headers) - 1) - headerBytes,
            0);
        if (received <= 0) {
            fprintf(stderr, "header receive failed: %d\n", WSAGetLastError());
            closesocket(socketValue);
            WSACleanup();
            return 6;
        }
        headerBytes += received;
        headers[headerBytes] = '\0';
    }
    const bool headersOk = strstr(headers, "200 OK") != nullptr &&
        strstr(headers, "Content-Type: audio/mpeg") != nullptr &&
        strstr(headers, "icy-metaint:") != nullptr;
    if (!headersOk) {
        fprintf(stderr, "invalid response headers\n");
        closesocket(socketValue);
        WSACleanup();
        return 7;
    }

    Sleep(stallMilliseconds);

    unsigned long long receivedBytes = 0;
    bool disconnected = false;
    char buffer[8192];
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(drainMilliseconds);
    while (GetTickCount64() < deadline) {
        const int received = recv(socketValue, buffer, sizeof(buffer), 0);
        if (received > 0) {
            receivedBytes += static_cast<unsigned long long>(received);
            continue;
        }
        if (received == 0) {
            disconnected = true;
            break;
        }
        const int error = WSAGetLastError();
        if (error != WSAETIMEDOUT && error != WSAEWOULDBLOCK) {
            disconnected = true;
            break;
        }
    }

    shutdown(socketValue, SD_BOTH);
    closesocket(socketValue);
    WSACleanup();
    printf(
        "STALL_MS=%d DRAIN_MS=%d RECEIVED_BYTES=%llu DISCONNECTED=%d\n",
        stallMilliseconds,
        drainMilliseconds,
        receivedBytes,
        disconnected ? 1 : 0);
    if (disconnected || receivedBytes < 128u * 1024u) {
        return 1;
    }
    printf("BACKPRESSURE_PROBE_PASS\n");
    return 0;
}
