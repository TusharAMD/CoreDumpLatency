#include <iostream>
#include <vector>
#include <cstring>
#include <chrono>
#include <thread>
#include <random>
#include <fstream>
#include <iomanip>
#include <unistd.h>

// Helper function to get current timestamp with milliseconds: HH:MM:SS.mmm
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
    std::string timestamped = "[" + get_timestamp() + "] [CRASHER] " + msg;
    std::cout << timestamped << std::endl;
    if (log_file.is_open()) {
        log_file << timestamped << std::endl;
        log_file.flush();
    }
}

int main(int argc, char* argv[]) {
    // Parse memory size in GB from argument (e.g., 1 = 1 GB, 0.5 = 500 MB, 0.2 = 200 MB)
    double mem_gb = 0.2;
    if (argc > 1) {
        try {
            mem_gb = std::stod(argv[1]);
        } catch (...) {
            mem_gb = 0.2;
        }
    }

    size_t alloc_bytes = static_cast<size_t>(mem_gb * 1024ULL * 1024ULL * 1024ULL);

    std::ofstream log_file("crasher.log", std::ios::out | std::ios::trunc);

    // Tell Linux kernel to dump all memory mappings
    FILE* fp = fopen("/proc/self/coredump_filter", "w");
    if (fp) {
        fprintf(fp, "0x3f\n");
        fclose(fp);
    }

    log_msg(log_file, "Process started (PID: " + std::to_string(getpid()) + ")");
    log_msg(log_file, "Allocating " + std::to_string(mem_gb).substr(0,4) + " GB (" + 
                      std::to_string(alloc_bytes / (1024 * 1024)) + " MB) of dirty RAM...");

    char* buffer = nullptr;
    try {
        buffer = new char[alloc_bytes];
        // Populate pages so they are non-zero and physically backed in RAM
        for (size_t i = 0; i < alloc_bytes; i += 4096) {
            buffer[i] = (char)(i & 0xFF);
            buffer[i + 1] = 0x55;
        }
    } catch (const std::exception& e) {
        log_msg(log_file, std::string("Allocation failed: ") + e.what());
        return 1;
    }

    log_msg(log_file, "Memory populated successfully (" + std::to_string(alloc_bytes / (1024 * 1024)) + " MB)!");

    // Random crash time between 5 and 60 seconds
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(5, 60);
    int target_second = dist(gen);

    log_msg(log_file, "Will run active CPU computation (NO SLEEP) and randomly dereference nullptr around second: " + std::to_string(target_second));

    auto start_time = std::chrono::steady_clock::now();
    int last_logged_second = 0;
    volatile uint64_t cpu_work_counter = 0;

    // Active CPU busy loop - keeps process in 100% 'R' (Running) state
    while (true) {
        // Continuous CPU work (math / hashing) to prevent sleeping
        for (int i = 0; i < 100000; ++i) {
            cpu_work_counter += (i * 31ULL) ^ 0x5DEECE66DULL;
        }

        auto now = std::chrono::steady_clock::now();
        int elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();

        // Log once per second
        if (elapsed > last_logged_second) {
            last_logged_second = elapsed;
            log_msg(log_file, "Active CPU working... elapsed: " + std::to_string(elapsed) + "s / " + std::to_string(target_second) + "s (ops: " + std::to_string(cpu_work_counter) + ")");
        }

        // Trigger crash when target second is reached
        if (elapsed >= target_second) {
            break;
        }
    }

    log_msg(log_file, ">>> TIME REACHED (" + std::to_string(target_second) + "s)! Dereferencing nullptr NOW! <<<");

    // Signal parent watcher via pipe descriptor 3 that crash is happening RIGHT NOW
    char crash_byte = 'X';
    write(3, &crash_byte, 1);
    close(3);

    // Null pointer dereference
    volatile int* ptr = nullptr;
    *ptr = 1234; // Segfault & kernel do_coredump() triggered here

    return 0;
}
