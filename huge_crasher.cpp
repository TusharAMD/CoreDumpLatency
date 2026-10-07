#include <iostream>
#include <vector>
#include <cstring>
#include <chrono>
#include <thread>
#include <random>
#include <unistd.h>

// Allocate 6 GB of dirty memory
const size_t ALLOC_SIZE = 6ULL * 1024ULL * 1024ULL * 1024ULL; // 6 GB

int main() {
    // Tell the kernel to dump EVERYTHING in this process: anonymous, shared, file-backed (mask 0x3f)
    FILE* fp = fopen("/proc/self/coredump_filter", "w");
    if (fp) {
        fprintf(fp, "0x3f\n");
        fclose(fp);
    }

    std::cout << "[CRASHER (PID " << getpid() << ")] Allocating 6 GB of dirty RAM..." << std::endl;
    
    char* buffer = nullptr;
    try {
        buffer = new char[ALLOC_SIZE];
        // Write distinct values across every 4KB page so memory is genuinely dirty & non-zero
        for (size_t i = 0; i < ALLOC_SIZE; i += 4096) {
            buffer[i] = (char)(i & 0xFF);
            buffer[i + 1] = 0x55;
        }
    } catch (const std::bad_alloc& e) {
        std::cerr << "Failed to allocate 4 GB, trying 2 GB..." << std::endl;
        size_t fallback_size = 2ULL * 1024ULL * 1024ULL * 1024ULL;
        buffer = new char[fallback_size];
        std::memset(buffer, 0xAA, fallback_size);
    }

    std::cout << "[CRASHER] Memory populated successfully in RAM!" << std::endl;

    // Random crash time between 5 and 60 seconds
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(5, 60);
    int target_second = dist(gen);

    std::cout << "[CRASHER] Will loop and randomly dereference nullptr around second: " << target_second << std::endl;

    auto start_time = std::chrono::steady_clock::now();
    for (int sec = 1; sec <= target_second; ++sec) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::cout << "[CRASHER] Working... elapsed: " << sec << "s / " << target_second << "s" << std::endl;
    }

    std::cout << "\n>>> [CRASHER] TIME REACHED (" << target_second 
              << "s)! Dereferencing nullptr to trigger core dump... <<<" << std::endl;
    std::cout.flush();

    volatile int* ptr = nullptr;
    *ptr = 1234; // Segfault triggers kernel do_coredump() here

    return 0;
}
