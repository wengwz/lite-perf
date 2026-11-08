#include <lite_perf/profiler.hpp>

#include <iostream>

int main() {
    int evaluations = 0;
    LITE_PERF_SCOPE((++evaluations, "disabled"));
    LITE_PERF_FUNC();
    // Arguments do not even need to be valid expressions in disabled mode.
    LITE_PERF_SCOPE(unavailable_label);
    if (evaluations != 0 || !lite_perf::Profiler::instance().snapshot().call_tree.empty()) {
        std::cerr << "Disabled instrumentation evaluated arguments or recorded samples\n";
        return 1;
    }
    if (!lite_perf::Profiler::instance().snapshot().threads.empty()) { return 1; }
    // Explicit APIs remain available even if the instrumentation macros are off.
    auto& profiler = lite_perf::Profiler::instance();
    { lite_perf::Scope scope("explicit"); }
    const auto snapshot = profiler.snapshot();
    return snapshot.call_tree.size() == 1 && snapshot.threads.size() == 1 &&
        snapshot.threads[0].roots == std::vector<std::size_t>{0} ? 0 : 1;
}
