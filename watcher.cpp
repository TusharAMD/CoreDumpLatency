#include <iostream>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <chrono>
#include <thread>
#include <fstream>
#include <string>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <atomic>
#include <csignal>

// Helper to get timestamp HH:MM:SS.mmm
std::string get_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf;
    localtime_r(&t, &tm_buf);

    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%H:%M:%S") << "." << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

void log_msg(std::ofstream& log_file, const std::string& msg) {
    std::string timestamped = "[" + get_timestamp() + "] " + msg;
    std::cout << timestamped << std::endl;
    if (log_file.is_open()) {
        log_file << timestamped << std::endl;
        log_file.flush();
    }
}

// Read process state from /proc/<pid>/stat
std::string get_process_state(pid_t pid) {
    std::string path = "/proc/" + std::to_string(pid) + "/stat";
    std::ifstream file(path);
    if (!file.is_open()) return "DEAD/NOT_FOUND";

    int p;
    std::string comm;
    std::string state;
    if (file >> p >> comm >> state) {
        return state;
    }
    return "UNKNOWN";
}

// Get file timestamps and size if it exists
std::string check_core_file(pid_t child_pid) {
    std::string core_path = "/tmp/core.huge_crasher." + std::to_string(child_pid);
    struct stat st;
    if (stat(core_path.c_str(), &st) == 0) {
        std::tm tm_buf;
        localtime_r(&st.st_mtime, &tm_buf);
        std::ostringstream oss;
        oss << "Found (" << (st.st_size / (1024 * 1024)) << " MB, Last-Modified: " 
            << std::put_time(&tm_buf, "%H:%M:%S") << ")";
        return oss.str();
    }
    return "Not created yet";
}

std::atomic<bool> keep_running{true};

void sigint_handler(int) {
    keep_running = false;
}

int main() {
    std::signal(SIGINT, sigint_handler);

    std::ofstream watcher_log("watcher.log", std::ios::out | std::ios::trunc);

    log_msg(watcher_log, "======================================================");
    log_msg(watcher_log, "[WATCHER] Starting watcher process (PID: " + std::to_string(getpid()) + ")");
    log_msg(watcher_log, "[WATCHER] Press Ctrl+C anytime to stop this watcher.");
    log_msg(watcher_log, "======================================================");

    pid_t child_pid = fork();

    if (child_pid == 0) {
        // Run huge_crasher
        char* args[] = { (char*)"./huge_crasher", nullptr };
        execv("./huge_crasher", args);
        perror("execv failed");
        _exit(1);
    }

    log_msg(watcher_log, "[WATCHER] Spawned child crasher PID: " + std::to_string(child_pid));

    // Background thread continuously monitoring child state and core file presence
    std::atomic<bool> child_exited{false};
    std::thread monitor([&]() {
        std::string last_state = "";
        while (!child_exited && keep_running) {
            std::string state = get_process_state(child_pid);
            std::string core_status = check_core_file(child_pid);

            // Log state with explanation
            std::string desc = "(?)";
            if (state == "R") desc = "(R: Running/CPU work or kernel dump in memory)";
            else if (state == "S") desc = "(S: Sleeping / nanosleep timer)";
            else if (state == "D") desc = "(D: Disk I/O uninterruptible sleep)";
            else if (state == "Z") desc = "(Z: Zombie / process finished)";
            else if (state == "DEAD/NOT_FOUND") desc = "(Terminated / cleaned up)";

            log_msg(watcher_log, "[MONITOR] PID " + std::to_string(child_pid) + 
                                 " State: " + state + " " + desc + 
                                 " | Core file: " + core_status);

            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    });

    // Wait for the child process to die
    int status = 0;
    auto start_wait = std::chrono::steady_clock::now();
    log_msg(watcher_log, "[WATCHER] Calling waitpid(" + std::to_string(child_pid) + ")... (blocked until kernel finishes exit)");

    pid_t w = waitpid(child_pid, &status, 0);
    auto end_wait = std::chrono::steady_clock::now();
    child_exited = true;

    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_wait - start_wait).count();

    log_msg(watcher_log, "\n======================================================");
    log_msg(watcher_log, "[WATCHER] waitpid() RETURNED! Process officially recognized as DEAD at: [" + get_timestamp() + "]");
    log_msg(watcher_log, "[WATCHER] waitpid() blocked for total of: " + std::to_string(duration_ms) + " ms (" + 
                         std::to_string(duration_ms / 1000.0) + " seconds)");

    if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        log_msg(watcher_log, "[WATCHER] Child terminated by signal: " + std::to_string(sig) + " (" + strsignal(sig) + ")");
        log_msg(watcher_log, "[WATCHER] Was core dump completed according to kernel? " + 
                             std::string(WCOREDUMP(status) ? "YES (WCOREDUMP=true)" : "NO"));
    }

    // Check final core file details
    std::string core_path = "/tmp/core.huge_crasher." + std::to_string(child_pid);
    struct stat st;
    if (stat(core_path.c_str(), &st) == 0) {
        std::tm tm_buf;
        localtime_r(&st.st_mtime, &tm_buf);
        std::ostringstream oss;
        oss << std::put_time(&tm_buf, "%H:%M:%S");
        log_msg(watcher_log, "[WATCHER] Core file path: " + core_path);
        log_msg(watcher_log, "[WATCHER] Core file size: " + std::to_string(st.st_size) + " bytes (" + 
                             std::to_string(st.st_size / (1024 * 1024)) + " MB)");
        log_msg(watcher_log, "[WATCHER] Core file last modified timestamp: [" + oss.str() + "]");
    } else {
        log_msg(watcher_log, "[WATCHER] Core file was NOT found on disk at " + core_path);
    }
    log_msg(watcher_log, "======================================================");

    if (monitor.joinable()) monitor.join();

    log_msg(watcher_log, "[WATCHER] Watcher is idle and staying alive as requested. Press Ctrl+C to exit.");
    while (keep_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    log_msg(watcher_log, "[WATCHER] Received Ctrl+C / SIGINT. Exiting watcher now. Goodbye!");
    return 0;
}
