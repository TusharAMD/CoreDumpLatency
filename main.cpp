#include <iostream>
#include <chrono>
#include <thread>
#include <random>

int main() {
    std::cout << "Starting countdown loop..." << std::endl;
    std::cout << "The program will randomly dereference a nullptr (guaranteed within 60s max)." << std::endl;

    // Random number generation
    std::random_device rd;
    std::mt19937 gen(rd());
    // Random target duration between 5 and 60 seconds
    std::uniform_int_distribution<int> crash_time_dist(5, 60);
    int target_crash_second = crash_time_dist(gen);

    std::cout << "[DEBUG] Scheduled to crash around second: " << target_crash_second << "\n" << std::endl;

    auto start_time = std::chrono::steady_clock::now();
    int elapsed_seconds = 0;

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        elapsed_seconds++;

        auto current_time = std::chrono::steady_clock::now();
        auto actual_elapsed = std::chrono::duration_cast<std::chrono::seconds>(current_time - start_time).count();

        std::cout << "Running... Elapsed: " << actual_elapsed << "s" << std::endl;

        // Trigger segmentation fault when elapsed matches target or hits safety limit
        if (actual_elapsed >= target_crash_second || actual_elapsed >= 60) {
            std::cout << "\n>>> Triggering nullptr dereference now! <<<" << std::endl;
            std::cout.flush();

            // Intentionally dereference a null pointer to produce SIGSEGV
            volatile int* null_ptr = nullptr;
            *null_ptr = 42; // Segmentation violation (SIGSEGV)
        }
    }

    return 0;
}
