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

std::string get_process_state(pid_t pid) {
    std::string path = "/proc/" + std::to_string(pid) + "/stat";
    std::ifstream file(path);
    if (!file.is_open()) return "DEAD/NOT_FOUND";
    int p;
    std::string comm, state;
    if (file >> p >> comm >> state) return state;
    return "UNKNOWN";
}

bool check_core_location(const std::string& dir, pid_t pid, off_t& out_size, std::string& out_path) {
    std::string path = dir + "/core.huge_crasher." + std::to_string(pid);
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        out_size = st.st_size;
        out_path = path;
        return true;
    }
    return false;
}

std::atomic<bool> keep_running{true};
void sigint_handler(int) { keep_running = false; }

int main(int argc, char* argv[]) {
    std::signal(SIGINT, sigint_handler);

    // Optional argument: "disk" (default: /tmp) or "ram" (/dev/shm)
    std::string mode = "disk";
    std::string core_dir = "/tmp";
    std::string mem_gb = "0.2"; // default 200 MB

    if (argc > 1) {
        std::string arg = argv[1];
        if (arg == "ram" || arg == "shm") {
            mode = "ram";
            core_dir = "/dev/shm";
        } else if (arg == "disk") {
            mode = "disk";
            core_dir = "/tmp";
        } else {
            core_dir = arg;
        }
    }

    if (argc > 2) {
        mem_gb = argv[2]; // e.g. 1, 0.5, 0.2
    }

    struct rlimit rl;
    rl.rlim_cur = RLIM_INFINITY;
    rl.rlim_max = RLIM_INFINITY;
    setrlimit(RLIMIT_CORE, &rl);

    std::ofstream watcher_log("watcher.log", std::ios::out | std::ios::trunc);

    log_msg(watcher_log, "==========================================================================");
    log_msg(watcher_log, "[WATCHER] Mode: " + mode + " | Target Memory: " + mem_gb + " GB | Core Dir: " + core_dir);
    log_msg(watcher_log, "==========================================================================");

    int crash_pipe[2];
    if (pipe(crash_pipe) < 0) perror("pipe failed");

    pid_t child_pid = fork();
    if (child_pid == 0) {
        // Child: set write-end of pipe to FD 3
        close(crash_pipe[0]);
        dup2(crash_pipe[1], 3);
        close(crash_pipe[1]);

        char* args[] = { (char*)"./huge_crasher", (char*)mem_gb.c_str(), nullptr };
        execv("./huge_crasher", args);
        perror("execv failed");
        _exit(1);
    }

    close(crash_pipe[1]); // Close write end in parent
    log_msg(watcher_log, "[WATCHER] Spawned child crasher PID: " + std::to_string(child_pid));

    std::atomic<bool> child_exited{false};
    std::atomic<bool> crash_occurred{false};
    std::chrono::system_clock::time_point t_spawn = std::chrono::system_clock::now();
    std::chrono::system_clock::time_point t_crash_exact;

    // Thread 1: listen for the exact crash byte from child
    std::thread crash_listener([&]() {
        char buf;
        if (read(crash_pipe[0], &buf, 1) > 0) {
            t_crash_exact = std::chrono::system_clock::now();
            crash_occurred = true;
        }
        close(crash_pipe[0]);
    });

    // Thread 2: monitor process states
    std::thread monitor([&]() {
        while (!child_exited && keep_running) {
            std::string state = get_process_state(child_pid);
            off_t current_core_size = 0;
            std::string path_found;
            bool exists = check_core_location(core_dir, child_pid, current_core_size, path_found);

            std::string desc = "(?)";
            if (state == "R") desc = "(R: Running / CPU work or Memory write)";
            else if (state == "S") desc = "(S: Sleeping)";
            else if (state == "D") desc = "(D: Disk I/O Uninterruptible Sleep)";
            else if (state == "DEAD/NOT_FOUND") desc = "(Cleaned up / gone)";

            std::string core_info = exists ? ("Found (" + std::to_string(current_core_size / (1024*1024)) + " MB)") 
                                           : "Not created yet";

            log_msg(watcher_log, "[MONITOR] PID " + std::to_string(child_pid) + " State: " + state + 
                                 " " + desc + " | Core: " + core_info);

            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    });

    log_msg(watcher_log, "[WATCHER] Calling waitpid(" + std::to_string(child_pid) + ")... (blocked)");

    int status = 0;
    pid_t w = waitpid(child_pid, &status, 0);
    std::chrono::system_clock::time_point t_waitpid_returned = std::chrono::system_clock::now();
    child_exited = true;

    if (crash_listener.joinable()) crash_listener.join();
    if (monitor.joinable()) monitor.join();

    off_t final_core_size = 0;
    std::string final_path;
    bool core_exists = check_core_location(core_dir, child_pid, final_core_size, final_path);

    double total_lifetime_sec = std::chrono::duration<double>(t_waitpid_returned - t_spawn).count();
    double dump_duration_sec = crash_occurred ? std::chrono::duration<double>(t_waitpid_returned - t_crash_exact).count() : 0.0;

    std::string child_sig = WIFSIGNALED(status) ? ("SIG " + std::to_string(WTERMSIG(status)) + " (" + strsignal(WTERMSIG(status)) + ")") : "Exited normally";
    std::string wcore_str = (WIFSIGNALED(status) && WCOREDUMP(status)) ? "YES (WCOREDUMP=true)" : "NO";

    std::ostringstream out;
    out << "\n"
        << "+=========================================================================================+\n"
        << "|                   PROCESS LIFECYCLE SUMMARY TABLE [TARGET: " << std::left << std::setw(5) << mode << "]                   |\n"
        << "+=========================================================================================+\n"
        << "| Metric / Event                       | Timestamp / Value                                |\n"
        << "+--------------------------------------+--------------------------------------------------+\n"
        << "| 1. Target Storage Medium             | " << std::left << std::setw(48) << (mode == "ram" ? "RAM (/dev/shm tmpfs)" : "DISK (/tmp physical storage)") << " |\n"
        << "| 2. Child Process Spawned             | " << std::left << std::setw(48) << (get_timestamp(t_spawn) + " (PID: " + std::to_string(child_pid) + ")") << " |\n"
        << "| 3. Crash Occurred (Exact Trigger)    | " << std::left << std::setw(48) << (crash_occurred ? get_timestamp(t_crash_exact) : "N/A") << " |\n"
        << "| 4. Watcher Notified of Death         | " << std::left << std::setw(48) << (get_timestamp(t_waitpid_returned) + " (via waitpid)") << " |\n"
        << "+--------------------------------------+--------------------------------------------------+\n"
        << "| 5. Time Spent Dumping Core           | " << std::left << std::setw(48) << (std::to_string(dump_duration_sec).substr(0,6) + " seconds (Exact Lag)") << " |\n"
        << "| 6. Total Process Lifetime            | " << std::left << std::setw(48) << (std::to_string(total_lifetime_sec).substr(0,6) + " seconds") << " |\n"
        << "+--------------------------------------+--------------------------------------------------+\n"
        << "| 7. Termination Signal                | " << std::left << std::setw(48) << child_sig << " |\n"
        << "| 8. Kernel Core Dump Flag             | " << std::left << std::setw(48) << wcore_str << " |\n"
        << "| 9. Core File Location                | " << std::left << std::setw(48) << (core_exists ? final_path : "None") << " |\n"
        << "| 10. Core File Size                   | " << std::left << std::setw(48) << (core_exists ? (std::to_string(final_core_size / (1024*1024)) + " MB (" + std::to_string(final_core_size) + " bytes)") : "0 MB") << " |\n"
        << "+=========================================================================================+\n";

    log_msg(watcher_log, out.str());
    log_msg(watcher_log, "[WATCHER] Watcher is idle and staying alive as requested. Press Ctrl+C to exit.");

    while (keep_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return 0;
}
