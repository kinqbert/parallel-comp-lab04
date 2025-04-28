import subprocess
import time

SERVER_EXECUTABLE = "server.exe"
CPP_CLIENT_EXECUTABLE = "client.exe"
NODE_CLIENT_COMMAND = "node node_client.js"

def open_terminal(command, title):
    subprocess.Popen(["start", "cmd", "/k", command], shell=True)

def main():
    cpp_clients = 1
    node_clients = 1

    print("[RUNNER] Starting server...")
    open_terminal(SERVER_EXECUTABLE, "Server")
    time.sleep(2) 

    for i in range(cpp_clients):
        print(f"[RUNNER] Starting C++ client #{i + 1}...")
        open_terminal(CPP_CLIENT_EXECUTABLE, f"C++ Client {i + 1}")
        time.sleep(0.1)

    for i in range(node_clients):
        print(f"[RUNNER] Starting Node.js client #{i + 1}...")
        open_terminal(NODE_CLIENT_COMMAND, f"Node Client {i + 1}")
        time.sleep(0.1)

if __name__ == "__main__":
    main()
