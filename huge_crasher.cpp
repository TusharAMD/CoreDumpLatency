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

// 2 GB allocation
const size_t ALLOC_SIZE = 2ULL * 1024ULL * 1024ULL * 1024ULL;

int main() {
    std::ofstream log_file("crasher.log", std::ios::out | std::ios::trunc);

    // Tell Linux kernel to dump all memory mappings (anonymous, file, shared)
    FILE* fp = fopen("/proc/self/coredump_filter", "w");
    if (fp) {
        fprintf(fp, "0x3f\n");
        fclose(fp);
    }

    log_msg(log_file, "Process started (PID: " + std::to_string(getpid()) + ")");
    log_msg(log_file, "Allocating 2 GB of dirty RAM...");

    char* buffer = nullptr;
    try {
        buffer = new char[ALLOC_SIZE];
        // Populate pages so they are non-zero and physically backed in RAM
        for (size_t i = 0; i < ALLOC_SIZE; i += 4096) {
            buffer[i] = (char)(i & 0xFF);
            buffer[i + 1] = 0x55;
        }
    } catch (const std::exception& e) {
        log_msg(log_file, std::string("Allocation failed: ") + e.what());
        return 1;
    }

    log_msg(log_file, "Memory populated successfully in RAM!");

    // Random crash time between 5 and 60 seconds
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(5, 60);
    int target_second = dist(gen);

    log_msg(log_file, "Will loop and randomly dereference nullptr around second: " + std::to_string(target_second));

    for (int sec = 1; sec <= target_second; ++sec) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        log_msg(log_file, "Working... elapsed: " + std::to_string(sec) + "s / " + std::to_string(target_second) + "s");
    }

    log_msg(log_file, ">>> TIME REACHED (" + std::to_string(target_second) + "s)! Dereferencing nullptr NOW! <<<");

    // Null pointer dereference
    volatile int* ptr = nullptr;
    *ptr = 1234; // Segfault & kernel do_coredump() triggered here

    return 0;
}
