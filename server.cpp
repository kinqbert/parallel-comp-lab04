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

constexpr char INIT[]   = "INIT";
constexpr char RUN[]    = "RUN";
constexpr char CHECK[]  = "CHECK";
constexpr char RESULT[] = "RESULT";

constexpr char OK[]   = "OK";
constexpr char BUSY[] = "BUSY";
constexpr char DONE[] = "DONE";
constexpr char ERR[]  = "ERR";

LONG WINAPI CrashHandler(EXCEPTION_POINTERS *ExceptionInfo) {
    cerr << "[CRASH] Unhandled exception occurred! Code: "
            << hex << ExceptionInfo->ExceptionRecord->ExceptionCode << endl;

    WSACleanup();

    return EXCEPTION_EXECUTE_HANDLER;
}

struct TaskInfo {
    uint32_t dimension = 0;
    vector<int32_t> matrix;
    uint32_t threadCount = 0;
    atomic<bool> running{false};
    atomic<bool> ready{false};
};

unordered_map<SOCKET, TaskInfo> tasks;
mutex tasksMutex;


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

void handleClient(const SOCKET client) {
    {
        lock_guard lock(tasksMutex);
        tasks.erase(client);
        tasks.emplace(piecewise_construct,
                      forward_as_tuple(client),
                      forward_as_tuple());
    }

    sendString(client, "Welcome to the server!");

    while (true) {
        std::string cmd;
        if (!recvString(client, cmd)) break;
        std::cout << "[SERVER] Received command: " << cmd << " from client " << client << '\n';

        try {
            if (cmd == INIT) {
                uint32_t threads, dimension;
                if (!recvData(client, &threads, 4) || !recvData(client, &dimension, 4)) {
                    throw runtime_error("Bad INIT header");
                }

                threads = ntohl(threads);
                dimension = ntohl(dimension);

                vector<int32_t> matrix(dimension * dimension);

                if (!recvData(client, matrix.data(), matrix.size() * 4)) {
                    throw runtime_error("Bad INIT data");
                }

                for (auto &v: matrix) {
                    v = ntohl(static_cast<uint32_t>(v));
                }

                {
                    lock_guard g(tasksMutex);
                    auto &task = tasks[client];
                    task.dimension = dimension;
                    task.matrix.swap(matrix);
                    task.threadCount = threads;
                    task.running = task.ready = false;
                }

                sendString(client, OK);
            } else if (cmd == RUN) {
                TaskInfo *taskPtr = nullptr;

                {
                    lock_guard lock(tasksMutex);
                    auto &t = tasks[client];

                    if (t.matrix.empty() || t.threadCount == 0) {
                        throw runtime_error("No INIT");
                    }

                    if (t.running) {
                        throw runtime_error("Already RUN");
                    }

                    t.running = true;
                    t.ready = false;

                    taskPtr = &t;
                }

                thread([taskPtr] {
                    const uint32_t dimension = taskPtr->dimension;
                    const uint32_t threads = taskPtr->threadCount;
                    const uint32_t rows = dimension / 2;
                    const uint32_t chunk = rows / threads;
                    const uint32_t extra = rows % threads;

                    auto work = [taskPtr, dimension](const uint32_t rowStart, const uint32_t rowEnd)
                    {
                        for (uint32_t i = rowStart; i < rowEnd; ++i) {
                            const int32_t mirrorRowIndex = dimension - 1 - i;
                            const int32_t* topRow = taskPtr->matrix.data() + i * dimension;
                            int32_t* bottomRow = taskPtr->matrix.data() + mirrorRowIndex * dimension;

                            for (uint32_t j = 0; j < dimension; ++j) {
                                bottomRow[j] = topRow[j];
                            }
                        }
                    };

                    vector<thread> pool;
                    uint32_t row0 = 0;

                    for (uint32_t p = 0; p < threads; ++p) {
                        uint32_t row1 = row0 + chunk + (p < extra ? 1 : 0);
                        pool.emplace_back(work, row0, row1);
                        row0 = row1;
                    }

                    for (auto &thread: pool) {
                        thread.join();
                    }

                    this_thread::sleep_for(chrono::milliseconds(2000));

                    cout << "[SERVER] Processing finished." << endl;

                    taskPtr->ready = true;
                    taskPtr->running = false;
                }).detach();

                sendString(client, OK);
            } else if (cmd == CHECK) {
                TaskInfo *task = nullptr;

                {
                    lock_guard lock(tasksMutex);
                    task = &tasks[client];
                }

                task->running ? sendString(client, BUSY)
                  : sendString(client, DONE);
            } else if (cmd == RESULT) {
                TaskInfo *task = nullptr;

                {
                    lock_guard lock(tasksMutex);
                    task = &tasks[client];
                }

                if (!task->ready) {
                    sendString(client, ERR);
                    continue;
                }

                sendString(client, DONE);

                uint32_t dimensionNet = htonl(task->dimension);
                sendData(client, &dimensionNet, 4);

                vector<int32_t> matrixNet(task->matrix.size());

                for (size_t i = 0; i < task->matrix.size(); ++i) {
                    matrixNet[i] = htonl(task->matrix[i]);
                }

                sendData(client, matrixNet.data(), matrixNet.size() * sizeof(int32_t));
            } else {
                throw runtime_error("Unknown cmd");
            }
        } catch (const exception &ex) {
            sendString(client, ERROR);
        }
    }

    closesocket(client);
    lock_guard lock(tasksMutex);
    tasks.erase(client);
}

int main() {
    SetUnhandledExceptionFilter(CrashHandler);

    WSADATA ws;
    if (WSAStartup(MAKEWORD(2, 2), &ws) != 0) {
        cerr << "WSAStartup failed\n";
        exit(1);
    }

    SOCKET listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == INVALID_SOCKET) {
        cerr << "socket() failed: " << WSAGetLastError() << "\n";
        WSACleanup();
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

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
