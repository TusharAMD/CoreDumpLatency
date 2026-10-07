#include <iostream>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <chrono>
#include <thread>
#include <fstream>
#include <string>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <atomic>
#include <csignal>

// Helper to get formatted timestamp HH:MM:SS.mmm
std::string get_timestamp(std::chrono::system_clock::time_point tp) {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()) % 1000;
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm_buf;
    localtime_r(&t, &tm_buf);

    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%H:%M:%S") << "." << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

std::string get_current_timestamp() {
    return get_timestamp(std::chrono::system_clock::now());
}

void log_msg(std::ofstream& log_file, const std::string& msg) {
    std::string timestamped = "[" + get_current_timestamp() + "] " + msg;
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

// Check core file existence, size, and mtime
bool check_core(pid_t pid, off_t& out_size, std::chrono::system_clock::time_point& out_tp) {
    std::string core_path = "/tmp/core.huge_crasher." + std::to_string(pid);
    struct stat st;
    if (stat(core_path.c_str(), &st) == 0) {
        out_size = st.st_size;
        out_tp = std::chrono::system_clock::from_time_t(st.st_mtime);
        return true;
    }
    return false;
}

std::atomic<bool> keep_running{true};
void sigint_handler(int) { keep_running = false; }

int main() {
    std::signal(SIGINT, sigint_handler);

    // Guarantee core dumps are enabled for this process tree
    struct rlimit rl;
    rl.rlim_cur = RLIM_INFINITY;
    rl.rlim_max = RLIM_INFINITY;
    setrlimit(RLIMIT_CORE, &rl);

    std::ofstream watcher_log("watcher.log", std::ios::out | std::ios::trunc);

    log_msg(watcher_log, "==========================================================================");
    log_msg(watcher_log, "[WATCHER] Starting watcher process (PID: " + std::to_string(getpid()) + ")");
    log_msg(watcher_log, "[WATCHER] Ensures RLIMIT_CORE = unlimited automatically.");
    log_msg(watcher_log, "==========================================================================");

    pid_t child_pid = fork();

    if (child_pid == 0) {
        char* args[] = { (char*)"./huge_crasher", nullptr };
        execv("./huge_crasher", args);
        perror("execv failed");
        _exit(1);
    }

    log_msg(watcher_log, "[WATCHER] Spawned child crasher PID: " + std::to_string(child_pid));

    // Variables for event timing
    std::atomic<bool> child_exited{false};
    std::atomic<bool> core_detected{false};
    std::chrono::system_clock::time_point t_spawn = std::chrono::system_clock::now();
    std::chrono::system_clock::time_point t_core_first_seen;
    off_t first_seen_size = 0;

    // Monitor thread to spot the exact millisecond the core dump begins
    std::thread monitor([&]() {
        while (!child_exited && keep_running) {
            std::string state = get_process_state(child_pid);
            off_t current_core_size = 0;
            std::chrono::system_clock::time_point mtime;
            bool exists = check_core(child_pid, current_core_size, mtime);

            if (exists && !core_detected) {
                core_detected = true;
                t_core_first_seen = std::chrono::system_clock::now();
                first_seen_size = current_core_size;
            }

            std::string desc = "(?)";
            if (state == "R") desc = "(R: Running / CPU work or Kernel dumping memory)";
            else if (state == "S") desc = "(S: Sleeping / nanosleep)";
            else if (state == "D") desc = "(D: Disk I/O Uninterruptible Sleep)";
            else if (state == "DEAD/NOT_FOUND") desc = "(Cleaned up / gone)";

            std::string core_info = exists ? ("Found (" + std::to_string(current_core_size / (1024*1024)) + " MB)") 
                                           : "Not created yet";

            log_msg(watcher_log, "[MONITOR] PID " + std::to_string(child_pid) + " State: " + state + 
                                 " " + desc + " | Core: " + core_info);

            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    });

    log_msg(watcher_log, "[WATCHER] Calling waitpid(" + std::to_string(child_pid) + ")... (blocked)");

    int status = 0;
    pid_t w = waitpid(child_pid, &status, 0);
    std::chrono::system_clock::time_point t_waitpid_returned = std::chrono::system_clock::now();
    child_exited = true;

    if (monitor.joinable()) monitor.join();

    // Check final core details
    off_t final_core_size = 0;
    std::chrono::system_clock::time_point core_mtime;
    bool core_exists = check_core(child_pid, final_core_size, core_mtime);

    // Calculate time differences
    double total_lifetime_sec = std::chrono::duration<double>(t_waitpid_returned - t_spawn).count();
    double dump_duration_sec = 0.0;
    if (core_detected) {
        dump_duration_sec = std::chrono::duration<double>(t_waitpid_returned - t_core_first_seen).count();
    }

    std::string child_sig = WIFSIGNALED(status) ? ("SIG " + std::to_string(WTERMSIG(status)) + " (" + strsignal(WTERMSIG(status)) + ")") : "Exited normally";
    std::string wcore_str = (WIFSIGNALED(status) && WCOREDUMP(status)) ? "YES (WCOREDUMP=true)" : "NO";

    // Build the formatted table
    std::ostringstream out;
    out << "\n"
        << "+=========================================================================================+\n"
        << "|                             PROCESS LIFECYCLE SUMMARY TABLE                             |\n"
        << "+=========================================================================================+\n"
        << "| Metric / Event                       | Timestamp / Value                                |\n"
        << "+--------------------------------------+--------------------------------------------------+\n"
        << "| 1. Child Process Spawned             | " << std::left << std::setw(48) << (get_timestamp(t_spawn) + " (PID: " + std::to_string(child_pid) + ")") << " |\n"
        << "| 2. Crash Occurred (Core Started)     | " << std::left << std::setw(48) << (core_detected ? (get_timestamp(t_core_first_seen) + " (First size: " + std::to_string(first_seen_size / (1024*1024)) + " MB)") : "N/A (No core)") << " |\n"
        << "| 3. Watcher Notified of Death         | " << std::left << std::setw(48) << (get_timestamp(t_waitpid_returned) + " (via waitpid)") << " |\n"
        << "+--------------------------------------+--------------------------------------------------+\n"
        << "| 4. Time Spent Dumping Core to Disk   | " << std::left << std::setw(48) << (core_detected ? (std::to_string(dump_duration_sec).substr(0,6) + " seconds (Lag/Delay)") : "0.000 seconds") << " |\n"
        << "| 5. Total Process Lifetime            | " << std::left << std::setw(48) << (std::to_string(total_lifetime_sec).substr(0,6) + " seconds") << " |\n"
        << "+--------------------------------------+--------------------------------------------------+\n"
        << "| 6. Termination Signal                | " << std::left << std::setw(48) << child_sig << " |\n"
        << "| 7. Kernel Core Dump Flag             | " << std::left << std::setw(48) << wcore_str << " |\n"
        << "| 8. Final Core File Path              | " << std::left << std::setw(48) << ("/tmp/core.huge_crasher." + std::to_string(child_pid)) << " |\n"
        << "| 9. Final Core File Size              | " << std::left << std::setw(48) << (core_exists ? (std::to_string(final_core_size / (1024*1024)) + " MB (" + std::to_string(final_core_size) + " bytes)") : "0 MB") << " |\n"
        << "+=========================================================================================+\n";

    log_msg(watcher_log, out.str());
    log_msg(watcher_log, "[WATCHER] Watcher is idle and staying alive as requested. Press Ctrl+C to exit.");

    while (keep_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    log_msg(watcher_log, "[WATCHER] Received Ctrl+C / SIGINT. Exiting watcher now. Goodbye!");
    return 0;
}
