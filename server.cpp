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

const char *getCommandName(const uint8_t cmd) {
    switch (cmd) {
        case CMD_INIT: return "CMD_INIT";
        case CMD_RUN: return "CMD_RUN";
        case CMD_CHECK: return "CMD_CHECK";
        case CMD_RESULT: return "CMD_RESULT";
        default: return "UNKNOWN_CMD";
    }
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

void handleClient(const SOCKET client) {
    {
        lock_guard lock(tasksMutex);
        tasks.erase(client);
        tasks.emplace(piecewise_construct,
                      forward_as_tuple(client),
                      forward_as_tuple());
    }

    while (true) {
        uint8_t cmd;
        if (!recvAll(client, &cmd, 1)) break;

        try {
            cout << "[SERVER] Received command: " << getCommandName(cmd)
                    << " from client " << client << endl;

            if (cmd == CMD_INIT) {
                uint32_t threads, dimension;
                if (!recvAll(client, &threads, 4) || !recvAll(client, &dimension, 4)) {
                    throw runtime_error("Bad INIT header");
                }

                threads = ntohl(threads);
                dimension = ntohl(dimension);

                vector<int32_t> matrix(dimension * dimension);

                if (!recvAll(client, matrix.data(), matrix.size() * 4)) {
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

                uint8_t rsp = RSP_OK;
                sendAll(client, &rsp, 1);
            } else if (cmd == CMD_RUN) {
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

                uint8_t rsp = RSP_OK;
                sendAll(client, &rsp, 1);
            } else if (cmd == CMD_CHECK) {
                TaskInfo *task;

                {
                    lock_guard lock(tasksMutex);
                    task = &tasks[client];
                }

                uint8_t rsp = task->running ? RSP_BUSY : RSP_DONE;
                sendAll(client, &rsp, 1);
            } else if (cmd == CMD_RESULT) {
                TaskInfo *task;

                {
                    lock_guard lock(tasksMutex);
                    task = &tasks[client];
                }

                if (!task->ready) {
                    throw runtime_error("Not ready");
                }

                uint8_t rsp = RSP_DONE;
                sendAll(client, &rsp, 1);

                uint32_t dimensionNet = htonl(task->dimension);
                sendAll(client, &dimensionNet, 4);

                vector<int32_t> matrixNet(task->matrix.size());

                for (size_t i = 0; i < task->matrix.size(); ++i) {
                    matrixNet[i] = htonl(task->matrix[i]);
                }

                sendAll(client, matrixNet.data(), matrixNet.size() * sizeof(int32_t));
            } else {
                throw runtime_error("Unknown cmd");
            }
        } catch (const exception &ex) {
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
