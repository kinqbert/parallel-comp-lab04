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
    CMD_INIT   = 0x01,
    CMD_RUN    = 0x02,
    CMD_CHECK  = 0x03,
    CMD_RESULT = 0x04
};
enum : uint8_t {
    RSP_OK    = 0x10,
    RSP_ERR   = 0x11,
    RSP_BUSY  = 0x12,
    RSP_DONE  = 0x13
};

bool recv_all(const SOCKET s, void* buf, int bytes) {
    auto ptr = static_cast<char*>(buf);
    while (bytes > 0) {
        const int r = recv(s, ptr, bytes, 0);
        if (r <= 0) return false;
        ptr += r;
        bytes -= r;
    }
    return true;
}

bool send_all(SOCKET s, const void* buf, int bytes) {
    const char* ptr = static_cast<const char*>(buf);
    while (bytes > 0) {
        int sent = send(s, ptr, bytes, 0);
        if (sent <= 0) return false;
        ptr += sent;
        bytes -= sent;
    }
    return true;
}

uint64_t ntoh64(uint64_t x) {
    uint32_t hi = ntohl(uint32_t(x >> 32));
    uint32_t lo = ntohl(uint32_t(x & 0xFFFFFFFF));
    return (uint64_t(hi) << 32) | lo;
}
uint64_t hton64(uint64_t x) {
    const uint32_t hi = htonl(static_cast<uint32_t>(x >> 32));
    const uint32_t lo = htonl(static_cast<uint32_t>(x & 0xFFFFFFFF));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

vector<int32_t> generateRandomPayload(size_t size, int32_t minValue = 1, int32_t maxValue = 100) {
    vector<int32_t> payload(size);

    random_device rd;
    mt19937 gen(rd());
    uniform_int_distribution dist(minValue, maxValue);

    for (auto& val : payload) {
        val = dist(gen);
    }

    return payload;
}


int main() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
        cerr << "WSAStartup failed\n";
        return 1;
    }

    const SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) {
        cerr << "socket() error\n";
        WSACleanup();
        return 1;
    }

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port   = htons(SERVER_PORT);
    inet_pton(AF_INET, "127.0.0.1", &serverAddr.sin_addr);

    const vector payload = generateRandomPayload(100);

    if (connect(sock, reinterpret_cast<sockaddr *>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR) {
        int err = WSAGetLastError();
        cerr << "connect() failed: " << err << "\n";
        closesocket(sock);
        WSACleanup();
        return 1;
    }


    {
        const uint32_t threadCount = 4;
        const uint8_t cmd = CMD_INIT;
        const uint32_t thr_n = htonl(threadCount);
        const uint32_t len_n = htonl(static_cast<uint32_t>(payload.size()));

        send_all(sock, &cmd, 1);
        send_all(sock, &thr_n, 4);
        send_all(sock, &len_n, 4);

        for (const int32_t v : payload) {
            int32_t tmp = htonl(v);
            send_all(sock, &tmp, 4);
        }

        uint8_t resp = 0;
        if (!recv_all(sock, &resp, 1) || resp != RSP_OK) {
            cerr << "INIT failed or server error\n";
            closesocket(sock);
            WSACleanup();
            return 1;
        }
        cout << "[CLIENT] INIT OK\n";
    }

    {
        uint8_t cmd = CMD_RUN;
        send_all(sock, &cmd, 1);

        uint8_t resp = 0;
        if (!recv_all(sock, &resp, 1) || resp != RSP_OK) {
            cerr << "RUN failed or server error\n";
            closesocket(sock);
            WSACleanup();
            return 1;
        }
        cout << "[CLIENT] RUN started\n";
    }

    while (true) {
        this_thread::sleep_for(chrono::milliseconds(500));
        uint8_t cmd = CMD_CHECK;
        send_all(sock, &cmd, 1);

        uint8_t resp = 0;
        if (!recv_all(sock, &resp, 1)) {
            cerr << "CHECK recv error\n";
            closesocket(sock);
            WSACleanup();
            return 1;
        }

        if (resp == RSP_BUSY) {
            cout << "[CLIENT] Server is still working...\n";
        }
        else if (resp == RSP_DONE) {
            cout << "[CLIENT] Server completed work\n";
            break;
        }
        else {
            cerr << "[CLIENT] Unexpected CHECK response: " << int(resp) << "\n";
            closesocket(sock);
            WSACleanup();
            return 1;
        }
    }

    {
        uint8_t cmd = CMD_RESULT;
        send_all(sock, &cmd, 1);

        uint8_t resp = 0;
        if (!recv_all(sock, &resp, 1) || resp != RSP_DONE) {
            cerr << "RESULT failed or server error\n";
            closesocket(sock);
            WSACleanup();
            return 1;
        }

        uint64_t netRes = 0;
        if (!recv_all(sock, &netRes, 8)) {
            cerr << "Failed to receive result value\n";
            closesocket(sock);
            WSACleanup();
            return 1;
        }
        const auto result = static_cast<int64_t>(ntoh64(netRes));
        cout << "[CLIENT] Computation result = " << result << "\n";


        int64_t expected = 0;
        for (int32_t val : payload)
            expected += val;

        if (result == expected) {
            cout << "[CLIENT] Result is correct!" << endl;
        } else {
            cerr << "[CLIENT] Mismatch! Expected " << expected << " but got " << result << endl;
        }

    }

    closesocket(sock);
    WSACleanup();
    return 0;
}
