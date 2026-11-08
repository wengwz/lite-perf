#include <lite_perf/profiler.hpp>

#include <chrono>
#include <iostream>
#include <thread>

using namespace std::chrono_literals;

void read_input() {
    LITE_PERF_FUNC();
    // Simulate work so the elapsed wall times are visible in the report.
    std::this_thread::sleep_for(1ms);
}

void validate_input() {
    LITE_PERF_FUNC();
    std::this_thread::sleep_for(1ms);
}

void load_input() {
    LITE_PERF_FUNC();
    read_input();
    validate_input();
}

void transform_input() {
    LITE_PERF_FUNC();
    // Every invocation gets its own node, including calls to the same function.
    validate_input();
    std::this_thread::sleep_for(2ms);
}

void save_output() {
    LITE_PERF_FUNC();
    std::this_thread::sleep_for(1ms);
}

void process_item() {
    LITE_PERF_FUNC();
    load_input();
    transform_input();
    save_output();
}

void run_batch() {
    LITE_PERF_FUNC();
    for (int i = 0; i < 3; ++i) {
        process_item();
    }
}

int main() {
    run_batch();
    // All measured scopes have ended before printing either report.
    auto& profiler = lite_perf::Profiler::instance();
    profiler.write_report(std::cout);
    std::cout << '\n';
    // Summarize completed calls across all threads and paths by label.
    profiler.write_summary_report(std::cout);
}
