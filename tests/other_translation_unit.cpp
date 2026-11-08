#include <lite_perf/profiler.hpp>

void record_from_other_translation_unit() {
    LITE_PERF_SCOPE("other translation unit");
}

lite_perf::Profiler* profiler_from_other_translation_unit() {
    return &lite_perf::Profiler::instance();
}
