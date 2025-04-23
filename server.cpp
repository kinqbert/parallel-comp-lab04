#define WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

#include <iostream>
#include <vector>
#include <thread>
#include <unordered_map>
#include <atomic>
#include <mutex>

#define PORT 8080

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


LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ExceptionInfo) {
    std::cerr << "[CRASH] Unhandled exception occurred! Code: "
              << std::hex << ExceptionInfo->ExceptionRecord->ExceptionCode << std::endl;

    WSACleanup();

    return EXCEPTION_EXECUTE_HANDLER;
}

struct TaskInfo {
    uint32_t N = 0;
    vector<int32_t> mtx;
    uint32_t threadCount = 0;
    atomic<bool> running{false};
    atomic<bool> ready  {false};
};

unordered_map<SOCKET, TaskInfo> tasks;
mutex tasksMutex;

uint64_t hton64(const uint64_t x) {
    const uint32_t hi = htonl(static_cast<uint32_t>(x >> 32));
    const uint32_t lo = htonl(static_cast<uint32_t>(x & 0xFFFFFFFF));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

bool recvAll(SOCKET s, void* buf, int len) {
    auto p = static_cast<char *>(buf);
    while (len > 0) {
        const int r = recv(s, p, len, 0);
        if (r <= 0) return false;
        p += r; len -= r;
    }
    return true;
}
bool sendAll(SOCKET s, const void* buf, int len) {
    auto p = static_cast<const char *>(buf);
    while (len > 0) {
        const int r = send(s, p, len, 0);
        if (r <= 0) return false;
        p += r; len -= r;
    }
    return true;
}

void initWinSock() {
    WSADATA ws;
    if (WSAStartup(MAKEWORD(2,2), &ws) != 0) {
        cerr << "WSAStartup failed\n";
        exit(1);
    }
}

void handleClient(const SOCKET client) {
    {
        lock_guard lock(tasksMutex);
        tasks.erase(client);
        tasks.emplace(std::piecewise_construct,
                      std::forward_as_tuple(client),
                      std::forward_as_tuple());
    }

    while (true) {
        uint8_t cmd;
        if (!recvAll(client, &cmd, 1)) break;

        try {
            cout << "[SERVER] Received command: " << hex << static_cast<int>(cmd) << " from client " << client << endl;

            if (cmd == CMD_INIT) {
                uint32_t thrN, dimN;
                if(!recvAll(client,&thrN,4) || !recvAll(client,&dimN,4))
                    throw runtime_error("Bad INIT header");
                thrN = ntohl(thrN);
                dimN = ntohl(dimN);

                vector<int32_t> buf(dimN*dimN);
                if(!recvAll(client,buf.data(),buf.size()*4))
                    throw runtime_error("Bad INIT data");
                for(auto& v:buf) v = ntohl(static_cast<uint32_t>(v));

                {
                    lock_guard g(tasksMutex);
                    auto& t = tasks[client];
                    t.N = dimN;
                    t.mtx.swap(buf);
                    t.threadCount = thrN;
                    t.running = t.ready = false;
                }
                uint8_t rsp = RSP_OK;
                sendAll(client,&rsp,1);
            }
            else if (cmd == CMD_RUN) {
                TaskInfo* tPtr = nullptr;
                {
                    lock_guard lock(tasksMutex);
                    auto& t = tasks[client];

                    if (t.mtx.empty() || t.threadCount == 0)
                        throw runtime_error("No INIT");

                    if (t.running)
                        throw runtime_error("Already RUN");

                    t.running = true;
                    t.ready   = false;

                    tPtr = &t;
                }

                thread([tPtr]{
                    const uint32_t N     = tPtr->N;
                    const uint32_t P     = tPtr->threadCount;
                    const uint32_t rows  = N/2;
                    const uint32_t chunk = rows / P;
                    const uint32_t extra = rows % P;

                    auto work = [tPtr,N](uint32_t rBegin,uint32_t rEnd){
                        for (uint32_t i = rBegin; i < rEnd; ++i) {
                            uint32_t mirror   = N - 1 - i;
                            int32_t* topRow   = tPtr->mtx.data() + i      * N;
                            int32_t* botRow   = tPtr->mtx.data() + mirror * N;
                            memcpy(botRow, topRow, N * sizeof(int32_t));
                        }
                    };

                    vector<thread> pool;
                    uint32_t r0 = 0;
                    for (uint32_t p = 0; p < P; ++p) {
                        uint32_t r1 = r0 + chunk + (p < extra ? 1 : 0);
                        pool.emplace_back(work, r0, r1);
                        r0 = r1;
                    }
                    for (auto& th : pool) th.join();

                    cout << "[SERVER] Processing finished." << endl;

                    tPtr->ready   = true;
                    tPtr->running = false;
                }).detach();

                uint8_t rsp = RSP_OK;
                sendAll(client, &rsp, 1);
            }
            else if (cmd == CMD_CHECK) {
                TaskInfo *t;
                {
                    lock_guard lock(tasksMutex);
                    t = &tasks[client];
                }
                uint8_t rsp = t->running ? RSP_BUSY : RSP_DONE;
                sendAll(client, &rsp, 1);
            }
            else if (cmd == CMD_RESULT) {
                TaskInfo *t;
                {
                    lock_guard lock(tasksMutex);
                    t = &tasks[client];
                }
                if (!t->ready)
                    throw runtime_error("Not ready");

                uint8_t rsp = RSP_DONE;
                sendAll(client,&rsp,1);

                uint32_t netN = htonl(t->N);
                sendAll(client,&netN,4);

                for(const int32_t v : t->mtx){
                    int32_t tmp = htonl(v);
                    sendAll(client,&tmp,4);
                }

            }
            else {
                throw runtime_error("Unknown cmd");
            }
        }
        catch (const exception& ex) {
            uint8_t rsp = RSP_ERR;
            sendAll(client, &rsp, 1);
        }
    }

    closesocket(client);
    lock_guard lock(tasksMutex);
    tasks.erase(client);
}

int main() {
    SetUnhandledExceptionFilter(CrashHandler);
    initWinSock();

    SOCKET listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == INVALID_SOCKET) {
        cerr << "socket() failed: " << WSAGetLastError() << "\n";
        WSACleanup();
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(PORT);

    if (bind(listener, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        cerr << "bind() failed: " << WSAGetLastError() << "\n";
        closesocket(listener);
        WSACleanup();
        return 1;
    }

    if (listen(listener, SOMAXCONN) == SOCKET_ERROR) {
        cerr << "listen() failed: " << WSAGetLastError() << "\n";
        closesocket(listener);
        WSACleanup();
        return 1;
    }

    cout << "[SERVER] listening on port " << PORT << "\n";

    while (true) {
        SOCKET client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            cerr << "accept() failed: " << WSAGetLastError() << "\n";
            break;
        }
        thread(handleClient, client).detach();
    }

    closesocket(listener);
    WSACleanup();
    return 0;
}
