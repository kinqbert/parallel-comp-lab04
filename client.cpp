#define WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <random>

#define SERVER_PORT 8080

using namespace std;

enum : uint8_t {
    CMD_INIT = 0x01,
    CMD_RUN = 0x02,
    CMD_CHECK = 0x03,
    CMD_RESULT = 0x04
};

enum : uint8_t {
    RSP_OK = 0x10,
    RSP_ERR = 0x11,
    RSP_BUSY = 0x12,
    RSP_DONE = 0x13
};

bool sendAll(SOCKET socket, const void *buf, int len) {
    auto p = static_cast<const char *>(buf);
    while (len) {
        const int n = send(socket, p, len, 0);
        if (n <= 0) return false;
        p += n;
        len -= n;
    }
    return true;
}

bool recvAll(SOCKET socket, void *buf, int len) {
    auto p = static_cast<char *>(buf);
    while (len) {
        const int n = recv(socket, p, len, 0);
        if (n <= 0) return false;
        p += n;
        len -= n;
    }
    return true;
}

vector<int32_t> makeMatrix(const uint32_t N, const int32_t lo = 1, const int32_t hi = 100) {
    vector<int32_t> m(N * N);
    random_device rd;
    mt19937 gen(rd());
    uniform_int_distribution dist(lo, hi);
    for (auto &v: m) v = dist(gen);
    return m;
}

void mirrorHorizontally(vector<int32_t> &m, const uint32_t N) {
    for (uint32_t i = 0; i < N / 2; ++i) {
        const uint32_t mir = N - 1 - i;
        const int32_t *top = m.data() + i * N;
        int32_t *bot = m.data() + mir * N;
        memcpy(bot, top, N * sizeof(int32_t));
    }
}

void printMatrix(const vector<int32_t> &m, uint32_t N, const string &title) {
    cout << title << " (" << N << "x" << N << "):\n";
    for (uint32_t i = 0; i < N; ++i) {
        for (uint32_t j = 0; j < N; ++j) {
            cout << setw(4) << m[i * N + j] << ' ';
        }
        cout << '\n';
    }
}

int main() {
    WSADATA ws;
    if (WSAStartup(MAKEWORD(2, 2), &ws) != 0) {
        cerr << "WSAStartup failed\n";
        return 1;
    }
    SOCKET sock = socket(AF_INET,SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) {
        cerr << "socket() failed: " << WSAGetLastError() << "\n";
        WSACleanup();
        return 1;
    }

    sockaddr_in srv{};
    srv.sin_family = AF_INET;
    srv.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, "127.0.0.1", &srv.sin_addr);

    if (connect(sock, reinterpret_cast<sockaddr *>(&srv), sizeof(srv)) == SOCKET_ERROR) {
        cerr << "connect() failed : " << WSAGetLastError() << "\n";
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    const int dimension = 10;
    const int THREADS = 2;
    const auto matrix = makeMatrix(dimension);

    const bool isSmallMatrix = dimension <= 10;

    if (isSmallMatrix) {
        printMatrix(matrix, dimension, "Original");
    }

    const uint8_t cmdInit = CMD_INIT;
    const int threadsNet = htonl(THREADS);
    const int dimensionNet = htonl(dimension);

    cout << "[CLIENT] SENDING DATA\n";

    sendAll(sock, &cmdInit, 1);
    sendAll(sock, &threadsNet, 4);
    sendAll(sock, &dimensionNet, 4);

    vector<int32_t> matrixNet(matrix.size());

    for (size_t i = 0; i < matrix.size(); ++i) {
        matrixNet[i] = htonl(matrix[i]);
    }

    sendAll(sock, matrixNet.data(), matrixNet.size() * sizeof(int32_t));

    uint8_t rsp;
    if (!recvAll(sock, &rsp, 1) || rsp != RSP_OK) {
        cerr << "INIT error\n";
        return 1;
    }
    cout << "[CLIENT] INIT OK\n";

    const uint8_t cmdRun = CMD_RUN;
    sendAll(sock, &cmdRun, 1);
    if (!recvAll(sock, &rsp, 1) || rsp != RSP_OK) {
        cerr << "RUN error\n";
        return 1;
    }
    cout << "[CLIENT] RUN accepted\n";

    while (true) {
        this_thread::sleep_for(chrono::milliseconds(500));
        uint8_t cmd = CMD_CHECK;
        sendAll(sock, &cmd, 1);
        uint8_t rspCheck;
        if (!recvAll(sock, &rspCheck, 1)) {
            cerr << "CHECK error\n";
            return 1;
        }
        if (rspCheck == RSP_BUSY) {
            cout << "[CLIENT] Server still working...\n";
            continue;
        }
        if (rspCheck == RSP_DONE) {
            cout << "[CLIENT] Computation finished\n";
            break;
        }
        cerr << "Unexpected CHECK response\n";
        return 1;
    }

    vector<int32_t> mirrored;
    uint32_t outN = 0;

    const uint8_t cmd = CMD_RESULT;
    sendAll(sock, &cmd, 1);

    uint8_t rspResult;

    if (!recvAll(sock, &rspResult, 1) || rspResult != RSP_DONE) {
        cerr << "RESULT error\n";
        return 1;
    }

    recvAll(sock, &outN, 4);
    outN = ntohl(outN);

    if (outN != dimension) {
        cerr << "Server sent matrix of different size\n";
        return 1;
    }

    mirrored.resize(dimension * dimension);

    for (auto &v : mirrored) {
        int32_t tmp;
        recvAll(sock, &tmp, 4);
        v = ntohl(tmp);
    }

    auto expected = matrix;

    if (isSmallMatrix) {
        mirrorHorizontally(expected, dimension);
        const bool ok = (mirrored == expected);

        printMatrix(expected, dimension, "Expected (mirrored locally)");
        printMatrix(mirrored, dimension, "Received from server");

        cout << (ok ? "[CLIENT] Mirror OK" : "[CLIENT] Mirror mismatch") << endl;
    }

    closesocket(sock);
    WSACleanup();
    return 0;
}
