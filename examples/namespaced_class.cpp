#include <lite_perf/profiler.hpp>

#include <chrono>
#include <iostream>
#include <thread>

using namespace std::chrono_literals;

namespace demo::io {

class Loader {
public:
    int load(int item_id) const {
        // Label: demo::io::Loader::load (no return type, parameters, or const).
        LITE_PERF_FUNC();
        std::this_thread::sleep_for(1ms);
        return item_id;
    }
};

} // namespace demo::io

namespace demo::pipeline {

class Processor {
public:
    int process(int item_id) const {
        // Calls across namespaces still form one nested call tree on this thread.
        LITE_PERF_FUNC();
        return transform(loader_.load(item_id));
    }

private:
    static int transform(int value) {
        // Static member functions also retain their namespace and class name.
        LITE_PERF_FUNC();
        std::this_thread::sleep_for(2ms);
        return value * 2;
    }

    io::Loader loader_;
};

int run_batch() {
    // A free function retains its namespace: demo::pipeline::run_batch.
    LITE_PERF_FUNC();
    Processor processor;
    int total = 0;
    for (int item_id = 1; item_id <= 3; ++item_id) {
        total += processor.process(item_id);
    }
    return total;
}

} // namespace demo::pipeline

int main() {
    const auto total = demo::pipeline::run_batch();
    std::cout << "Batch result: " << total << "\n\n";

    // All measured scopes have ended; durations are displayed in milliseconds.
    auto& profiler = lite_perf::Profiler::instance();
    profiler.write_report(std::cout, false, lite_perf::TimeUnit::Milliseconds);
    std::cout << '\n';
    profiler.write_summary_report(std::cout, lite_perf::TimeUnit::Milliseconds);
}
