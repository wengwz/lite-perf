#include <lite_perf/profiler.hpp>

#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

void process_item() {
    // Function timing includes load, compute, and the overhead of submitting their samples.
    LITE_PERF_FUNC();
    {
        LITE_PERF_SCOPE("load");
        // Simulate I/O with sleep to demonstrate elapsed wall time rather than CPU time.
        std::this_thread::sleep_for(1ms);
    }
    {
        LITE_PERF_SCOPE("compute");
        std::this_thread::sleep_for(2ms);
    }
}

int main() {
    auto& profiler = lite_perf::Profiler::instance();
    {
        // batch times the parallel batch; each worker invocation gets a separate node.
        LITE_PERF_SCOPE("batch");
        std::vector<std::thread> workers;
        for (int i = 0; i < 4; ++i) {
            workers.emplace_back([] {
                for (int j = 0; j < 10; ++j) {
                    process_item();
                }
            });
        }
        for (auto& worker : workers) {
            // After join, worker scopes have been destroyed and their samples submitted.
            worker.join();
        }
    }
    // Print after leaving the batch block so its completed sample is included.
    // Worker roots are separate from batch: call context stays on its originating thread.
    profiler.write_report(std::cout);
    std::cout << '\n';
    profiler.write_summary_report(std::cout);
}
