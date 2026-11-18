# lite-perf

English | [简体中文](README_zh.md)

A C++17 header-only profiling library with no third-party dependencies. Each instrumented function or code block invocation produces a separate call-tree node with its own duration and parent ID. Supports recording from multiple threads without an external profiler.

## Quick start

Copy `include/lite_perf/profiler.hpp` into your project's include path:

```cpp
#include <lite_perf/profiler.hpp>
#include <iostream>

void process() {
    LITE_PERF_FUNC(); // Use the current function name as the label.
    {
        LITE_PERF_SCOPE("load");
        // Load data here.
    } // Automatically record the load duration.
    {
        LITE_PERF_SCOPE("compute");
        // Compute here.
    }
} // Record process duration, including on return or exception.

int main() {
    for (int i = 0; i < 100; ++i) {
        process();
    }
    lite_perf::Profiler::instance().write_report(std::cout);
}
```

Compile with `g++ -std=c++17 -O2 -pthread -Iinclude your_program.cpp -o your_program`.

Example output (actual timings depend on the measured code):

```text
lite-perf call tree (inclusive wall time, seconds)
Thread 12345
Name                               Duration (s)    Parent (%)      Status
├── "process"                           0.000300           N/A   completed
│   ├── "load"                          0.000080        26.67%   completed
│   └── "compute"                       0.000200        66.67%   completed
└── "process"                           0.000320           N/A   completed
    ├── "load"                          0.000080        25.00%   completed
    └── "compute"                       0.000210        65.62%   completed
... further invocations ...
```

Reports group individual invocations by thread, using `tree`-style `├──`, `└──`, and `│` continuation lines to show branches, plus duration as a percentage of the parent. Siblings stay in node creation order; repeated names appear on separate rows. Durations are stored in nanoseconds and displayed in seconds by default; nanosecond units do not imply nanosecond clock resolution.

## Singleton and early stopping

All macros and scopes record to `Profiler::instance()`. `Profiler` has a private constructor and destructor and cannot be copied or moved; users cannot create separate collectors.

```cpp
auto& profiler = lite_perf::Profiler::instance();
{
    LITE_PERF_SCOPE("request");
    // Process the request here.
}
profiler.write_report(std::cout);
```

`LITE_PERF_FUNC()` records the qualified function name, such as `MyClass::run` or `my_namespace::MyClass::run`, without the return type, parameter list, or trailing qualifiers such as `const`. GCC/Clang (`__PRETTY_FUNCTION__`) and MSVC (`__FUNCSIG__`) provide the class/namespace information; unsupported signature formats and other compilers fall back to `__func__`. Template class names retain their template arguments. Overloads with the same qualified name are merged in the summary; use custom labels such as `LITE_PERF_SCOPE("MyClass::run_int")` to distinguish them. For more control over measurement intervals:

```cpp
lite_perf::Scope scope("partial scope");
// Run the work being measured here.
scope.stop(); // End early; later calls or destruction do not submit again.
```

## Snapshots

```cpp
auto snapshot = profiler.snapshot();
for (const auto& thread : snapshot.threads) {
    // thread.thread_id identifies the recorded thread.
    for (auto root_id : thread.roots) {
        const auto& root = snapshot.call_tree[root_id];
        // Each recorded root belongs to this thread, including completed roots.
    }
}
for (const auto& node : snapshot.call_tree) {
    // node.id, node.parent_id, node.name, node.duration_ns, node.completed
    if (node.completed) {
        // duration_ns is this invocation's inclusive wall time.
    }
}
```

Snapshots own both `call_tree` records and `threads` metadata, copied under the same mutex, and are unaffected by later records. Each call creates a node immediately. Active calls have `completed == false` and `duration_ns == 0`; stopping sets the duration and marks completion. A completed zero-duration call is distinguishable from an active call. Records persist for the singleton lifetime; there is no reset API.

## Scope call tree

The singleton keeps a vector call stack and a persistent root-ID vector for each thread ID. Stack entries are removed when scopes stop; root lists remain available after scopes complete and threads exit. `Snapshot::threads` lists threads by their first root's creation order, with roots in creation order. Reused `std::thread::id` values share a report group. Print the per-thread invocation trees with:

```cpp
{
    LITE_PERF_SCOPE("request");
    {
        LITE_PERF_SCOPE("load");
        // Load data here.
    }
    {
        LITE_PERF_SCOPE("compute");
        // Compute here.
    }
}
profiler.write_report(std::cout);
// Or retain the data for custom rendering:
auto snapshot = profiler.snapshot();
for (const auto& node : snapshot.call_tree) {
    // node.id, node.parent_id, node.name, node.duration_ns, node.completed
    // parent_id == id denotes a root.
}
```

`write_report(out, bool verbose = false, TimeUnit unit = TimeUnit::Seconds)` prints per-thread call trees with `tree`-style Unicode branches. Rows contain the invocation's name, inclusive duration, parent percentage, and completion status. Passing `true` also includes `ID` and `Parent ID` columns. `Snapshot::write_report` accepts the same parameter.

Both reports automatically expand the name column for long labels (including escaped characters). Call tree reports also account for nesting depth, keeping subsequent columns aligned.

- The parent percentage is `100 * child.duration_ns / parent.duration_ns`, shown with two decimal places. It is `N/A` for roots, unfinished calls, unfinished parents, or zero-duration parents. Unfinished durations also display `N/A`.
- If a parent was explicitly stopped early, a completed child's percentage may exceed 100%; it is not clamped.
- Every invocation remains a distinct node, including repeated names and paths, recursive calls, and calls on different threads. Siblings and roots remain in creation order inside each thread group.
- The parent is the nearest active scope on the same thread. New threads do not inherit context; cross-thread task context propagation is not provided.
- Stopping a parent early preserves existing children's parent IDs. Subsequent scopes attach to the nearest remaining active scope; non-top stops erase the corresponding ID from the vector.
- IDs start at 0 and index `call_tree`. A root's `parent_id == id`. Only instrumented scope nesting is recorded.
- All calls and persistent root lists remain until singleton destruction; memory grows with the total number of invocations.

## Report time units

Both report methods accept `lite_perf::TimeUnit`: `Seconds` (`s`, the default), `Milliseconds` (`ms`), `Microseconds` (`us`), or `Nanoseconds` (`ns`). Titles and column headers show the selected unit. Values use six decimal places; parent percentages are independent of this setting. Stored `duration_ns` values remain unchanged.

```cpp
using lite_perf::TimeUnit;
profiler.write_report(std::cout); // Seconds; hide IDs.
profiler.write_report(std::cout, true, TimeUnit::Milliseconds);
profiler.write_summary_report(std::cout, TimeUnit::Microseconds);

// Saved snapshots accept the same options.
auto saved = profiler.snapshot();
saved.write_report(std::cout, false, TimeUnit::Nanoseconds);
saved.write_summary_report(std::cout, TimeUnit::Seconds);
```

## Summary report

```cpp
profiler.write_report(std::cout);         // Call trees grouped by thread; hide IDs.
profiler.write_report(std::cout, true);   // Also show ID and Parent ID columns.
profiler.write_summary_report(std::cout); // Same-name statistics across all threads.

// Both reports can also use the same saved snapshot:
auto saved = profiler.snapshot();
saved.write_report(std::cout);
saved.write_summary_report(std::cout);
```

The summary merges labels across all threads and paths, sorting rows by total duration in descending order, then by name for equal totals. `Calls`, `Total (s)`, `Min (s)`, `Max (s)`, and `Mean (s)` use **completed calls only**. `Incomplete` counts unfinished invocations separately; their durations never enter the aggregates. A label with only unfinished calls has zero calls and total time, and `N/A` minimum, maximum, and mean. Completed zero-duration calls participate normally. Reports preserve the caller's stream flags and precision.

## Disable instrumentation

Compile with `-DLITE_PERF_ENABLED=0`, or define it before the first include:

```cpp
#define LITE_PERF_ENABLED 0
#include <lite_perf/profiler.hpp>
```

`LITE_PERF_SCOPE` and `LITE_PERF_FUNC` then become no-ops. Their arguments are not evaluated, and they do not construct timers or access collectors. Explicit `Scope` construction remains available. Keep the macro setting consistent across the project to avoid conflicting definitions of instrumented inline functions.

## Measurement semantics and overhead

- Uses `std::chrono::steady_clock` to measure elapsed wall time, including sleep, I/O, and lock waits. These measurements do not directly indicate CPU utilization. Nanoseconds are the storage unit, not a clock resolution guarantee.
- Each node records one invocation's inclusive duration. Normally a parent includes its child durations; explicitly stopping a parent early ends its interval immediately. Overlapping intervals must not be added to infer program runtime. Exclusive time is not computed.
- `snapshot()` and report methods support concurrent calls. Registration and completion update nodes and thread stacks under one mutex. Construct, stop, and destroy each `Scope` on the same thread. Callers must synchronize shared output streams.
- Print final reports after scopes end and workers join. Macros declare local variables: use braces around conditional and loop bodies.
- Labels are copied into nodes. Registration allocations, label copying, and lock acquisition are excluded from that scope's timing; child bookkeeping contributes to enclosing scope durations. Completion lock waits are excluded from the completing scope's duration.
- `Scope::stop()` and destruction do not throw and submit at most once. Completion errors are suppressed to keep destruction non-throwing; an unsuccessful completion can leave its node active. Construction, snapshots, and output may throw on allocation or stream errors. Failed registration rolls back its node and stack entry.
- The singleton owns all records directly. Join worker threads and finish all scopes before singleton destruction; avoid first accessing it during global object destruction.

## Build examples and tests

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/profiler_example
./build/profiler_multilevel_example
./build/profiler_namespaced_class_example
```

`examples/multi_level.cpp` demonstrates four levels of function calls: `run_batch → process_item → load_input → read_input`. It prints the thread-grouped call tree and a summary. The tree retains six separate `validate_input` calls; the summary combines them into one row with six completed calls.

`examples/namespaced_class.cpp` demonstrates nested namespaces, classes, const member functions, and a static member function. Labels such as `demo::pipeline::Processor::process` retain namespace and class names without return types or parameter lists. Both reports use milliseconds.

To use the library in an existing CMake project:

```cmake
set(LITE_PERF_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LITE_PERF_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(path/to/lite-perf)
target_link_libraries(your_target PRIVATE lite_perf::lite_perf)
```

Tests cover per-invocation durations, repeated names and paths, RAII, exceptions and early returns, repeated stops, label ownership, recursion, vector growth, non-top stops, thread isolation, concurrent snapshots, cross-translation-unit nesting, persistent thread roots, parent percentages, completed-only summaries, and disabled argument evaluation.
