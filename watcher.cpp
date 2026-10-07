#include <iostream>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <chrono>
#include <thread>
#include <fstream>
#include <string>
#include <cstring>

// Helper to inspect process state in /proc/<pid>/stat
std::string get_process_state(pid_t pid) {
    std::string path = "/proc/" + std::to_string(pid) + "/stat";
    std::ifstream file(path);
    if (!file.is_open()) return "DEAD/NOT_FOUND";
    
    // /proc/[pid]/stat format: pid (comm) state ...
    int p;
    std::string comm;
    std::string state;
    if (file >> p >> comm >> state) {
        return state; // R = Running, S = Sleeping, D = Disk/Uninterruptible sleep, Z = Zombie
    }
    return "UNKNOWN";
}

int main() {
    std::cout << "======================================================" << std::endl;
    std::cout << "[WATCHER] Starting watcher process (PID: " << getpid() << ")" << std::endl;
    std::cout << "======================================================" << std::endl;

    pid_t pid = fork();

    if (pid == 0) {
        // Child process
        char* args[] = { (char*)"./huge_crasher", nullptr };
        execv("./huge_crasher", args);
        perror("execv failed");
        _exit(1);
    }

    std::cout << "[WATCHER] Spawned child crasher with PID: " << pid << std::endl;

    // Monitor child state in background thread
    bool finished = false;
    std::thread monitor([&]() {
        while (!finished) {
            std::string state = get_process_state(pid);
            if (state == "DEAD/NOT_FOUND" && finished) break;
            std::cout << "[MONITOR] Child state: " << state 
                      << " (D=Uninterruptible/Kernel dump, S=Sleeping, R=Running, Z=Zombie)" 
                      << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    });

    int status = 0;
    auto start_wait = std::chrono::steady_clock::now();
    std::cout << "[WATCHER] Entering waitpid(" << pid << ")..." << std::endl;

    // waitpid blocks until the child process officially exits
    pid_t w = waitpid(pid, &status, 0);
    auto end_wait = std::chrono::steady_clock::now();
    finished = true;
    if (monitor.joinable()) monitor.join();

    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_wait - start_wait).count();

    std::cout << "\n======================================================" << std::endl;
    std::cout << "[WATCHER] waitpid() finally unblocked after: " << duration_ms << " ms (" 
              << (duration_ms / 1000.0) << " seconds)!" << std::endl;

    if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        std::cout << "[WATCHER] Child killed by signal: " << sig << " (" << strsignal(sig) << ")" << std::endl;
        std::cout << "[WATCHER] Was core dump created? " 
                  << (WCOREDUMP(status) ? "YES (WCOREDUMP=true)" : "NO") << std::endl;
    }
    std::cout << "======================================================" << std::endl;

    return 0;
}
