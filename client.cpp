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

constexpr char INIT[]   = "INIT";
constexpr char RUN[]    = "RUN";
constexpr char CHECK[]  = "CHECK";
constexpr char RESULT[] = "RESULT";

constexpr char OK[]   = "OK";
constexpr char BUSY[] = "BUSY";
constexpr char DONE[] = "DONE";
constexpr char ERR[]  = "ERR";

bool sendData(const SOCKET socket, const void *buf, int len) {
    auto p = static_cast<const char *>(buf);
    while (len) {
        const int n = send(socket, p, len, 0);
        if (n <= 0) return false;
        p += n;
        len -= n;
    }
    return true;
}

bool recvData(const SOCKET socket, void *buf, int len) {
    auto p = static_cast<char *>(buf);
    while (len) {
        const int n = recv(socket, p, len, 0);
        if (n <= 0) return false;
        p += n;
        len -= n;
    }
    return true;
}

bool sendString(const SOCKET s, const std::string& msg) {
    const uint32_t n = htonl(static_cast<uint32_t>(msg.size()));
    return sendData(s, &n, 4) && sendData(s, msg.data(), msg.size());
}

bool recvString(const SOCKET s, std::string& out) {
    uint32_t nNet;
    if (!recvData(s, &nNet, 4)) return false;
    const uint32_t n = ntohl(nNet);
    out.resize(n);
    return recvData(s, out.data(), n);
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

    std::string greet;
    recvString(sock, greet);
    std::cout << "[CLIENT] Server says: " << greet << '\n';

    const int dimension = 10;
    const int THREADS = 2;
    const auto matrix = makeMatrix(dimension);

    const bool isSmallMatrix = dimension <= 10;

    if (isSmallMatrix) {
        printMatrix(matrix, dimension, "Original");
    }

    const string cmdInit = "INIT";
    const int threadsNet = htonl(THREADS);
    const int dimensionNet = htonl(dimension);

    cout << "[CLIENT] SENDING DATA\n";

    sendString(sock, "INIT");
    sendData(sock, &threadsNet, 4);
    sendData(sock, &dimensionNet, 4);

    vector<int32_t> matrixNet(matrix.size());

    for (size_t i = 0; i < matrix.size(); ++i) {
        matrixNet[i] = htonl(matrix[i]);
    }

    sendData(sock, matrixNet.data(), matrixNet.size() * sizeof(int32_t));

    string rsp;
    if (!recvString(sock, rsp) || rsp != OK) {
        cerr << "INIT error: " << rsp << '\n';
        return 1;
    }
    cout << "[CLIENT] INIT OK\n";

    const string cmdRun = RUN;
    sendString(sock, cmdRun);
    if (!recvString(sock, rsp) || rsp != OK) {
        cerr << "RUN error\n";
        return 1;
    }
    cout << "[CLIENT] RUN accepted\n";

    while (true) {
        this_thread::sleep_for(chrono::milliseconds(500));

        sendString(sock, CHECK);

        string rspCheck;
        if (!recvString(sock, rspCheck)) {
            cerr << "CHECK error\n";
            return 1;
        }

        if (rspCheck == BUSY) {
            cout << "[CLIENT] Server still working...\n";
            continue;
        }
        if (rspCheck == DONE) {
            cout << "[CLIENT] Computation finished\n";
            break;
        }

        cerr << "Unexpected CHECK response: " << rspCheck << "\n";
        return 1;
    }

    vector<int32_t> mirrored;
    uint32_t outN = 0;

    string rspResult;
    sendString(sock, "RESULT");

    if (!recvString(sock, rspResult)) {
        cerr << "RESULT error (no response)\n";
        return 1;
    }

    if (rspResult != DONE) {
        cerr << "RESULT error: " << rspResult << "\n";
        return 1;
    }

    recvData(sock, &outN, 4);
    outN = ntohl(outN);

    if (outN != dimension) {
        cerr << "Server sent matrix of different size\n";
        return 1;
    }

    mirrored.resize(dimension * dimension);

    for (auto &v : mirrored) {
        int32_t tmp;
        recvData(sock, &tmp, 4);
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
