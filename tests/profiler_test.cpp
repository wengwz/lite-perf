#include <lite_perf/profiler.hpp>

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

using namespace std::chrono_literals;

static_assert(!std::is_default_constructible_v<lite_perf::Profiler>);
static_assert(!std::is_copy_constructible_v<lite_perf::Profiler>);
static_assert(!std::is_move_constructible_v<lite_perf::Profiler>);
static_assert(!std::is_destructible_v<lite_perf::Profiler>);

void record_from_other_translation_unit();
lite_perf::Profiler* profiler_from_other_translation_unit();

// Keep checks active in Release builds too.
#define CHECK(condition) do { \
    if (!(condition)) { \
        std::cerr << __FILE__ << ':' << __LINE__ << ": " #condition "\n"; \
        std::abort(); \
    } \
} while (false)

std::vector<const lite_perf::CallTreeNode*> named(
    const lite_perf::Snapshot& snapshot, std::string_view name) {
    std::vector<const lite_perf::CallTreeNode*> result;
    for (const auto& node : snapshot.call_tree) {
        if (node.name == name) { result.push_back(&node); }
    }
    return result;
}

const lite_perf::CallTreeNode& single(const lite_perf::Snapshot& snapshot, std::string_view name) {
    const auto nodes = named(snapshot, name);
    CHECK(nodes.size() == 1);
    return *nodes.front();
}

void check_integrity(const lite_perf::Snapshot& snapshot) {
    std::vector<std::size_t> owners(snapshot.call_tree.size(), snapshot.threads.size());
    std::size_t previous_first = 0;
    for (std::size_t i = 0; i < snapshot.threads.size(); ++i) {
        const auto& thread = snapshot.threads[i];
        CHECK(!thread.roots.empty());
        if (i != 0) { CHECK(previous_first < thread.roots.front()); }
        previous_first = thread.roots.front();
        for (std::size_t j = 0; j < thread.roots.size(); ++j) {
            const auto root = thread.roots[j];
            CHECK(root < snapshot.call_tree.size());
            CHECK(snapshot.call_tree[root].parent_id == root);
            CHECK(owners[root] == snapshot.threads.size());
            owners[root] = i;
            if (j != 0) { CHECK(thread.roots[j - 1] < root); }
        }
    }
    for (std::size_t i = 0; i < snapshot.call_tree.size(); ++i) {
        const auto& node = snapshot.call_tree[i];
        CHECK(node.id == i);
        CHECK(node.parent_id <= node.id);
        CHECK(node.completed || node.duration_ns == 0);
        if (node.parent_id != node.id) { owners[i] = owners[node.parent_id]; }
        CHECK(owners[i] < snapshot.threads.size()); // Every invocation belongs to a recorded thread root.
    }
}

void test_invocations_and_snapshot() {
    auto& profiler = lite_perf::Profiler::instance();
    CHECK(profiler.snapshot().call_tree.empty());
    {
        lite_perf::Scope sample("sample");
        std::this_thread::sleep_for(1ms);
    }
    {
        lite_perf::Scope sample("sample");
        std::this_thread::sleep_for(3ms);
    }
    const auto saved = profiler.snapshot();
    CHECK(saved.call_tree.size() == 2);
    CHECK(saved.call_tree[0].id != saved.call_tree[1].id);
    CHECK(saved.call_tree[0].name == "sample" && saved.call_tree[1].name == "sample");
    CHECK(saved.call_tree[0].duration_ns >= 1000000);
    CHECK(saved.call_tree[1].duration_ns >= 3000000);
    CHECK(saved.call_tree[0].completed && saved.call_tree[1].completed);
    CHECK(saved.call_tree[0].parent_id == saved.call_tree[0].id &&
          saved.call_tree[1].parent_id == saved.call_tree[1].id);

    lite_perf::Scope active("active sample");
    const auto live = profiler.snapshot();
    CHECK(!single(live, "active sample").completed);
    CHECK(single(live, "active sample").duration_ns == 0);
    active.stop();
    const auto stopped = profiler.snapshot();
    const auto duration = single(stopped, "active sample").duration_ns;
    CHECK(single(stopped, "active sample").completed);
    active.stop();
    CHECK(single(profiler.snapshot(), "active sample").duration_ns == duration);
    CHECK(!single(live, "active sample").completed); // Saved data is immutable.
    CHECK(saved.call_tree.size() == 2);
    CHECK(saved.threads.size() == 1 && saved.threads[0].roots.size() == 2);
    CHECK(live.threads[0].roots.size() == 3);
    CHECK(saved.threads[0].thread_id == std::this_thread::get_id());
    check_integrity(stopped);
}

void test_lifetime_and_macros() {
    auto& profiler = lite_perf::Profiler::instance();
    CHECK(profiler_from_other_translation_unit() == &profiler);
    {
        LITE_PERF_FUNC();
        std::string label = "owned label";
        lite_perf::Scope child(label);
        label.assign("changed");
        std::this_thread::sleep_for(1ms);
    }
    try {
        LITE_PERF_SCOPE("exception");
        throw std::runtime_error("expected");
    } catch (const std::runtime_error&) {}
    const auto early_return = [] {
        LITE_PERF_SCOPE("return");
        return 7;
    };
    CHECK(early_return() == 7);
    int evaluations = 0;
    { LITE_PERF_SCOPE((++evaluations, "evaluated label")); }
    CHECK(evaluations == 1);
    {
        LITE_PERF_SCOPE("first"); LITE_PERF_SCOPE("second");
    }
    {
        LITE_PERF_SCOPE("cross TU parent");
        record_from_other_translation_unit();
    }
    const auto snapshot = profiler.snapshot();
    const auto& child = single(snapshot, "owned label");
    const auto& parent = snapshot.call_tree[child.parent_id];
    CHECK(parent.name.find("test_lifetime_and_macros") != std::string::npos);
    CHECK(child.parent_id == parent.id);
    CHECK(child.duration_ns >= 1000000 && parent.duration_ns >= child.duration_ns);
    CHECK(single(snapshot, "exception").completed);
    CHECK(single(snapshot, "return").completed);
    CHECK(single(snapshot, "second").parent_id == single(snapshot, "first").id);
    CHECK(single(snapshot, "other translation unit").parent_id == single(snapshot, "cross TU parent").id);
    check_integrity(snapshot);
}

void record_recursive(int depth) {
    LITE_PERF_FUNC();
    if (depth > 1) { record_recursive(depth - 1); }
}

void test_repeated_paths_and_recursion() {
    auto& profiler = lite_perf::Profiler::instance();
    for (int i = 0; i < 2; ++i) {
        lite_perf::Scope parent("repeated parent");
        for (int j = 0; j < 3; ++j) { lite_perf::Scope child("repeated child"); }
    }
    constexpr int depth = 64;
    record_recursive(depth); // Also exercise node and stack vector reallocations.
    const auto snapshot = profiler.snapshot();
    const auto parents = named(snapshot, "repeated parent");
    const auto children = named(snapshot, "repeated child");
    CHECK(parents.size() == 2 && children.size() == 6);
    for (std::size_t i = 0; i < parents.size(); ++i) {
        CHECK(parents[i]->parent_id == parents[i]->id);
        for (std::size_t j = 0; j < 3; ++j) {
            CHECK(children[i * 3 + j]->parent_id == parents[i]->id);
            CHECK(children[i * 3 + j]->completed);
        }
    }
    std::vector<const lite_perf::CallTreeNode*> recursive;
    for (const auto& node : snapshot.call_tree) {
        if (node.name.find("record_recursive") != std::string::npos) { recursive.push_back(&node); }
    }
    CHECK(recursive.size() == depth);
    for (std::size_t i = 0; i < recursive.size(); ++i) {
        CHECK(recursive[i]->parent_id == (i == 0 ? recursive[i]->id : recursive[i - 1]->id));
        CHECK(recursive[i]->completed);
        if (i > 0) { CHECK(recursive[i - 1]->duration_ns >= recursive[i]->duration_ns); }
    }
    check_integrity(snapshot);
}

void test_thread_stack_lifetime() {
    auto& profiler = lite_perf::Profiler::instance();
    constexpr int rounds = 3;
    constexpr int thread_count = 4;
    for (int round = 0; round < rounds; ++round) {
        std::vector<std::thread> threads;
        for (int i = 0; i < thread_count; ++i) {
            threads.emplace_back([] {
                auto outer = std::make_unique<lite_perf::Scope>("stack outer");
                auto middle = std::make_unique<lite_perf::Scope>("stack middle");
                lite_perf::Scope inner("stack inner");
                middle.reset();
                { lite_perf::Scope leaf("stack leaf"); }
                inner.stop();
                { lite_perf::Scope child("stack next child"); }
                outer.reset();
                { lite_perf::Scope root("stack new root"); }
            });
        }
        for (auto& thread : threads) { thread.join(); }
    }
    const auto snapshot = profiler.snapshot();
    CHECK(named(snapshot, "stack outer").size() == rounds * thread_count);
    CHECK(named(snapshot, "stack new root").size() == rounds * thread_count);
    for (const auto& node : snapshot.call_tree) {
        if (node.name == "stack outer" || node.name == "stack new root") {
            CHECK(node.parent_id == node.id);
        } else if (node.name == "stack leaf") {
            const auto& inner = snapshot.call_tree[node.parent_id];
            CHECK(inner.name == "stack inner");
            CHECK(snapshot.call_tree[inner.parent_id].name == "stack middle");
        } else if (node.name == "stack next child") {
            CHECK(snapshot.call_tree[node.parent_id].name == "stack outer");
        }
        CHECK(node.completed);
    }
    check_integrity(snapshot);
}

void test_threads_and_live_snapshots() {
    auto& profiler = lite_perf::Profiler::instance();
    constexpr int thread_count = 4;
    constexpr int iterations = 200;
    std::vector<std::thread::id> worker_ids(thread_count);
    std::vector<std::size_t> worker_roots(thread_count);
    std::atomic<int> started{0};
    std::atomic<int> done{0};
    std::atomic<bool> finish{false};
    std::vector<std::thread> threads;
    {
        lite_perf::Scope main("main thread");
        for (int i = 0; i < thread_count; ++i) {
            threads.emplace_back([&, i] {
                worker_ids[i] = std::this_thread::get_id();
                lite_perf::Scope worker("worker");
                const auto live = profiler.snapshot();
                for (const auto& thread : live.threads) {
                    if (thread.thread_id == worker_ids[i]) { worker_roots[i] = thread.roots.back(); }
                }
                ++started;
                while (!finish.load()) { std::this_thread::yield(); }
                for (int j = 0; j < iterations; ++j) { lite_perf::Scope child("thread child"); }
                ++done;
            });
        }
        while (started.load() != thread_count) { std::this_thread::yield(); }
        const auto live = profiler.snapshot();
        const auto workers = named(live, "worker");
        CHECK(workers.size() == thread_count);
        for (int i = 0; i < thread_count; ++i) {
            bool found = false;
            for (const auto& thread : live.threads) {
                if (thread.thread_id == worker_ids[i]) {
                    CHECK(std::find(thread.roots.begin(), thread.roots.end(), worker_roots[i]) != thread.roots.end());
                    CHECK(live.call_tree[worker_roots[i]].name == "worker");
                    found = true;
                }
            }
            CHECK(found);
        }
        for (const auto* worker : workers) {
            CHECK(worker->parent_id == worker->id && !worker->completed);
        }
        finish.store(true);
        while (done.load() != thread_count) {
            check_integrity(profiler.snapshot());
            std::this_thread::yield();
        }
        for (auto& thread : threads) { thread.join(); }
    }
    const auto snapshot = profiler.snapshot();
    CHECK(named(snapshot, "thread child").size() == thread_count * iterations);
    for (const auto& node : snapshot.call_tree) {
        if (node.name == "thread child") {
            CHECK(snapshot.call_tree[node.parent_id].name == "worker");
        }
        CHECK(node.completed);
    }
    check_integrity(snapshot);
}

void test_reports() {
    lite_perf::Snapshot snapshot;
    snapshot.call_tree = {
        {0, 0, "parent", 4000000, true},
        {1, 0, "child", 1000000, true},
        {2, 2, "other root", 0, true},
        {3, 0, "child", 2000000, true},
        {4, 4, "active", 0, false},
        {5, 4, "child", 2000000, true}, // Completed child, unfinished parent.
        {6, 0, "child", 9000000, false}, // Ignore unfinished durations in both reports.
        {7, 2, "child", 0, true}, // Completed zero-duration parent and child.
        {8, 0, "long", 5000000, true}, // An early-stopped parent can be shorter than a child.
        {9, 0, "zero", 0, true}
    };
    snapshot.threads = {{std::this_thread::get_id(), {0, 4}}, {std::thread::id{}, {2}}};
    std::ostringstream tree;
    tree << std::scientific << std::setprecision(2);
    const auto flags = tree.flags();
    snapshot.write_report(tree);
    CHECK(tree.flags() == flags && tree.precision() == 2);
    CHECK(tree.str().find("Parent (%)") != std::string::npos);
    CHECK(tree.str().find("Parent ID") == std::string::npos);

    const std::vector<std::vector<std::size_t>> groups{{0, 1, 3, 6, 8, 9, 4, 5}, {2, 7}};
    const std::vector<std::string> percentages{
        "N/A", "25.00%", "N/A", "50.00%", "N/A", "N/A", "N/A", "N/A", "125.00%", "0.00%"};
    const std::vector<std::string> prefixes{
        "├── ", "│   ├── ", "└── ", "│   ├── ", "└── ", "    └── ", "│   ├── ", "    └── ", "│   ├── ", "│   └── "};
    std::string line;
    for (bool verbose : {false, true}) {
        std::ostringstream out;
        out << std::scientific << std::setprecision(2);
        snapshot.write_report(out, verbose);
        CHECK(out.flags() == flags && out.precision() == 2);
        if (!verbose) { CHECK(out.str() == tree.str()); }
        std::istringstream rows(out.str());
        std::getline(rows, line); // Title.
        for (std::size_t i = 0; i < groups.size(); ++i) {
            std::ostringstream heading;
            heading << "Thread " << snapshot.threads[i].thread_id;
            CHECK(static_cast<bool>(std::getline(rows, line)) && line == heading.str());
            CHECK(static_cast<bool>(std::getline(rows, line))); // Columns.
            CHECK((line.find("Parent ID") != std::string::npos) == verbose);
            for (auto expected_id : groups[i]) {
                CHECK(static_cast<bool>(std::getline(rows, line)));
                const auto& prefix = prefixes[expected_id];
                CHECK(line.substr(0, prefix.size()) == prefix);
                std::istringstream row(line.substr(prefix.size()));
                std::string name, duration, percentage, status;
                CHECK(static_cast<bool>(row >> std::quoted(name)));
                const auto& node = snapshot.call_tree[expected_id];
                CHECK(name == node.name);
                if (verbose) {
                    std::size_t id = 0, parent = 0;
                    CHECK(static_cast<bool>(row >> id >> parent));
                    CHECK(id == node.id && parent == node.parent_id);
                }
                CHECK(static_cast<bool>(row >> duration >> percentage >> status));
                if (node.completed) {
                    CHECK(std::stod(duration) == static_cast<double>(node.duration_ns) / 1e9);
                } else {
                    CHECK(duration == "N/A");
                }
                // ASCII labels plus Unicode tree branches must leave numeric columns aligned.
                const auto after_label = line.find('"', prefix.size() + 1) + 1;
                const auto field = line.find_first_not_of(' ', after_label);
                std::size_t columns = 0;
                for (std::size_t j = 0; j < field; ++j) {
                    if ((static_cast<unsigned char>(line[j]) & 0xc0) != 0x80) { ++columns; }
                }
                CHECK(columns == (verbose ? 44 - std::to_string(node.id).size() : 49 - duration.size()));
                CHECK(percentage == percentages[expected_id]);
                CHECK(status == (node.completed ? "completed" : "active"));
                CHECK(!(row >> name)); // Hidden columns must not leave extra fields.
            }
        }
        CHECK(!std::getline(rows, line));
    }

    std::ostringstream summary;
    summary << std::scientific << std::setprecision(2);
    snapshot.write_summary_report(summary);
    CHECK(summary.flags() == flags && summary.precision() == 2);
    std::istringstream summary_rows(summary.str());
    std::getline(summary_rows, line);
    std::getline(summary_rows, line);
    std::size_t seen = 0;
    const std::vector<std::string> summary_order{"child", "long", "parent", "active", "other root", "zero"};
    while (std::getline(summary_rows, line)) {
        std::istringstream row(line);
        std::string name, total, min, max, mean;
        std::uint64_t count = 0, incomplete = 0;
        CHECK(static_cast<bool>(row >> std::quoted(name) >> count >> total >> min >> max >> mean >> incomplete));
        CHECK(seen < summary_order.size() && name == summary_order[seen]);
        if (name == "child") {
            CHECK(count == 4 && incomplete == 1);
            CHECK(total == "0.005000" && min == "0.000000" && max == "0.002000" && mean == "0.001250");
        } else if (name == "active") {
            CHECK(count == 0 && incomplete == 1 && total == "0.000000");
            CHECK(min == "N/A" && max == "N/A" && mean == "N/A");
        } else if (name == "zero" || name == "other root") {
            CHECK(count == 1 && incomplete == 0 && total == "0.000000");
            CHECK(min == "0.000000" && max == "0.000000" && mean == "0.000000");
        } else {
            CHECK(count == 1 && incomplete == 0);
        }
        ++seen;
    }
    CHECK(seen == 6);

    snapshot.call_tree.push_back({10, 0, "child\n\t\"\\", 0, true});
    std::ostringstream escaped, escaped_summary;
    snapshot.write_report(escaped);
    snapshot.write_summary_report(escaped_summary);
    CHECK(escaped.str().find("└── \"child\\n\\t\\\"\\\\\"") != std::string::npos);
    CHECK(escaped_summary.str().find("\"child\\n\\t\\\"\\\\\"") != std::string::npos);

    auto& profiler = lite_perf::Profiler::instance();
    std::ostringstream direct_tree, saved_tree, direct_summary, saved_summary;
    for (bool verbose : {false, true}) {
        direct_tree.str("");
        saved_tree.str("");
        profiler.write_report(direct_tree, verbose);
        profiler.snapshot().write_report(saved_tree, verbose);
        CHECK(direct_tree.str() == saved_tree.str());
    }
    profiler.write_summary_report(direct_summary);
    profiler.snapshot().write_summary_report(saved_summary);
    CHECK(direct_summary.str() == saved_summary.str());

    lite_perf::Snapshot branches;
    branches.call_tree = {
        {0, 0, "root A", 0, true}, {1, 0, "branch A", 0, true}, {2, 1, "leaf A", 0, true},
        {3, 0, "branch B", 0, true}, {4, 3, "leaf B", 0, true}, {5, 5, "root B", 0, true}
    };
    branches.threads = {{std::this_thread::get_id(), {0, 5}}};
    std::ostringstream branch_report;
    branches.write_report(branch_report);
    CHECK(branch_report.str().find("│   │   └── \"leaf A\"") != std::string::npos);
    CHECK(branch_report.str().find("│       └── \"leaf B\"") != std::string::npos);
    CHECK(branch_report.str().find("\n└── \"root B\"") != std::string::npos);

    lite_perf::Snapshot empty;
    std::ostringstream empty_report, empty_summary;
    empty.write_report(empty_report);
    empty.write_summary_report(empty_summary);
    CHECK(empty_report.str().find("Thread ") == std::string::npos);
    CHECK(empty_summary.str().find("Incomplete") != std::string::npos);
}

void test_report_units() {
    using lite_perf::TimeUnit;
    lite_perf::Snapshot snapshot;
    snapshot.call_tree = {
        {0, 0, "parent", 2000000000, true},
        {1, 0, "child", 125000000, true},
        {2, 0, "child", 375000000, true},
        {3, 0, "child", 0, false}
    };
    snapshot.threads = {{std::this_thread::get_id(), {0}}};
    struct UnitCase { TimeUnit unit; const char* symbol; double divisor; };
    for (const auto& test : {
        UnitCase{TimeUnit::Seconds, "s", 1e9}, UnitCase{TimeUnit::Milliseconds, "ms", 1e6},
        UnitCase{TimeUnit::Microseconds, "us", 1e3}, UnitCase{TimeUnit::Nanoseconds, "ns", 1.0}
    }) {
        std::ostringstream report, summary;
        report << std::scientific << std::setprecision(2);
        summary << std::scientific << std::setprecision(2);
        const auto flags = report.flags();
        snapshot.write_report(report, true, test.unit);
        snapshot.write_summary_report(summary, test.unit);
        CHECK(report.flags() == flags && report.precision() == 2);
        CHECK(summary.flags() == flags && summary.precision() == 2);
        CHECK(report.str().find(std::string("Duration (") + test.symbol + ")") != std::string::npos);
        for (const auto* column : {"Total", "Min", "Max", "Mean"}) {
            CHECK(summary.str().find(std::string(column) + " (" + test.symbol + ")") != std::string::npos);
        }
        if (test.unit == TimeUnit::Seconds) {
            std::ostringstream default_report, default_summary;
            snapshot.write_report(default_report, true);
            snapshot.write_summary_report(default_summary);
            CHECK(default_report.str() == report.str());
            CHECK(default_summary.str() == summary.str());
        }
        std::istringstream rows(report.str());
        std::string line;
        std::getline(rows, line); // Title.
        std::getline(rows, line); // Thread.
        std::getline(rows, line); // Columns.
        for (const auto& node : snapshot.call_tree) {
            CHECK(static_cast<bool>(std::getline(rows, line)));
            std::istringstream row(line.substr(line.find('"')));
            std::string name, duration, percent, status;
            std::size_t id = 0, parent = 0;
            CHECK(static_cast<bool>(row >> std::quoted(name) >> id >> parent >> duration >> percent >> status));
            CHECK(id == node.id && parent == node.parent_id);
            if (node.completed) {
                CHECK(std::stod(duration) == node.duration_ns / test.divisor);
            } else {
                CHECK(duration == "N/A");
            }
            CHECK(percent == (id == 1 ? "6.25%" : id == 2 ? "18.75%" : "N/A"));
        }
        std::istringstream summary_rows(summary.str());
        std::getline(summary_rows, line);
        std::getline(summary_rows, line);
        CHECK(static_cast<bool>(std::getline(summary_rows, line)));
        std::istringstream row(line);
        std::string name;
        std::uint64_t count = 0, incomplete = 0;
        double total = 0, min = 0, max = 0, mean = 0;
        CHECK(static_cast<bool>(row >> std::quoted(name) >> count >> total >> min >> max >> mean >> incomplete));
        CHECK(name == "parent" && count == 1 && incomplete == 0);
        CHECK(total == 2000000000 / test.divisor && min == total && max == total && mean == total);

        auto& profiler = lite_perf::Profiler::instance();
        std::ostringstream direct, saved, direct_summary, saved_summary;
        profiler.write_report(direct, false, test.unit);
        profiler.snapshot().write_report(saved, false, test.unit);
        CHECK(direct.str() == saved.str());
        profiler.write_summary_report(direct_summary, test.unit);
        profiler.snapshot().write_summary_report(saved_summary, test.unit);
        CHECK(direct_summary.str() == saved_summary.str());
    }
}

void test_report_name_width() {
    lite_perf::Snapshot snapshot;
    snapshot.call_tree = {
        {0, 0, "root", 1000000000, true},
        {1, 0, "a_function_name_that_is_much_longer_than_thirty_two_characters", 200000000, true},
        {2, 0, std::string(40, '\t') + "\"\\\n", 0, false}
    };
    // Deep indentation must expand the column even when every label is short.
    for (std::size_t id = 3; id < 28; ++id) {
        snapshot.call_tree.push_back({id, id - 1, "nested", 1000000, true});
    }
    snapshot.call_tree.push_back({28, 28, "other thread", 0, true});
    snapshot.threads = {{std::this_thread::get_id(), {0}}, {std::thread::id{}, {28}}};

    // Count Unicode tree glyphs as one display column each; labels here are ASCII.
    const auto columns = [](std::string_view text) {
        std::size_t count = 0;
        for (unsigned char c : text) {
            if ((c & 0xc0) != 0x80) { ++count; }
        }
        return count;
    };
    for (bool verbose : {false, true}) {
        std::ostringstream out;
        snapshot.write_report(out, verbose);
        std::istringstream rows(out.str());
        std::string line;
        std::size_t expected_width = 0, seen = 0, headers = 0;
        while (std::getline(rows, line)) {
            if (line.rfind("Name", 0) == 0) {
                if (headers == 0) { expected_width = columns(line); }
                CHECK(columns(line) == expected_width);
                ++headers;
            } else if (line.find('"') != std::string::npos) {
                CHECK(columns(line) == expected_width);
                // Numeric columns have fixed widths, so their right edges must match the header.
                const auto suffix = line.substr(line.rfind('"') + 1);
                CHECK(suffix.size() >= (verbose ? 68 : 45));
                CHECK(suffix.front() == ' ');
                ++seen;
            }
        }
        CHECK(headers == 2 && seen == snapshot.call_tree.size());
    }
    std::ostringstream summary;
    snapshot.write_summary_report(summary);
    std::istringstream rows(summary.str());
    std::string line;
    std::getline(rows, line); // Title.
    std::getline(rows, line); // Columns.
    const auto expected_width = columns(line);
    std::size_t seen = 0;
    while (std::getline(rows, line)) {
        CHECK(columns(line) == expected_width);
        ++seen;
    }
    CHECK(seen == 5);
}

namespace profile_fixture {
struct Worker {
    void run() const { LITE_PERF_FUNC(); }
    void run(int) { LITE_PERF_FUNC(); }
    static void dispatch() { LITE_PERF_FUNC(); }
};
struct Other {
    void run() { LITE_PERF_FUNC(); }
};
template <typename T, typename U>
struct Box {
    Box() { LITE_PERF_FUNC(); }
    ~Box() { LITE_PERF_FUNC(); }
    int operator()(int value) const { LITE_PERF_FUNC(); return value; }
    explicit operator bool() const { LITE_PERF_FUNC(); return true; }
    template <typename V>
    V convert(V value) const { LITE_PERF_FUNC(); return value; }
};
} // namespace profile_fixture

void test_member_function_names() {
    auto& profiler = lite_perf::Profiler::instance();
    const auto start = profiler.snapshot().call_tree.size();
    profile_fixture::Worker worker;
    worker.run();
    worker.run(1);
    profile_fixture::Worker::dispatch();
    profile_fixture::Other{}.run();
    const auto snapshot = profiler.snapshot();
    CHECK(snapshot.call_tree.size() == start + 4);
#if defined(_MSC_VER) || defined(__clang__) || defined(__GNUC__)
    const std::vector<std::string> qualified_names{
        "profile_fixture::Worker::run", "profile_fixture::Worker::run",
        "profile_fixture::Worker::dispatch", "profile_fixture::Other::run"};
    for (std::size_t i = 0; i < qualified_names.size(); ++i) {
        const auto& node = snapshot.call_tree[start + i];
        CHECK(node.name == qualified_names[i]);
        CHECK(node.completed);
        CHECK(named(snapshot, node.name).size() == (i < 2 ? 2 : 1));
    }
    std::ostringstream summary;
    snapshot.write_summary_report(summary);
    CHECK(summary.str().find("\"profile_fixture::Worker::run\"") != std::string::npos);
#else
    CHECK(snapshot.call_tree[start].name == "run");
    CHECK(snapshot.call_tree[start + 2].name == "dispatch");
#endif
}

void test_function_name_extraction() {
    using lite_perf::detail::qualified_function_name;
    CHECK(qualified_function_name("int ns::Worker::run(double) const", "run") == "ns::Worker::run");
    CHECK(qualified_function_name("void __cdecl ns::Worker::run(int)", "run") == "ns::Worker::run");
    CHECK(qualified_function_name("const char *ns::name()", "name") == "ns::name");
    CHECK(qualified_function_name("int ns::Box<int, double>::operator()(int) const", "operator()")
          == "ns::Box<int, double>::operator()");
    CHECK(qualified_function_name("ns::Box<int, double>::operator bool() const", "operator bool")
          == "ns::Box<int, double>::operator bool");
    CHECK(qualified_function_name("T ns::Box<T, U>::convert(V) const [with V = int]", "convert")
          == "ns::Box<T, U>::convert");
    CHECK(qualified_function_name("int __cdecl ns::convert<int>(int)", "convert") == "ns::convert");
    CHECK(qualified_function_name("void (*ns::factory())()", "factory") == "ns::factory");
    CHECK(qualified_function_name("void (anonymous namespace)::run()", "run") == "(anonymous namespace)::run");
    CHECK(qualified_function_name("unrecognized signature", "run") == "run");

    auto& profiler = lite_perf::Profiler::instance();
    const auto start = profiler.snapshot().call_tree.size();
    {
        profile_fixture::Box<int, double> box;
        CHECK(box(5) == 5);
        CHECK(static_cast<bool>(box));
        CHECK(box.convert(7) == 7);
    }
    const auto snapshot = profiler.snapshot();
    CHECK(snapshot.call_tree.size() == start + 5);
#if defined(_MSC_VER) || defined(__clang__) || defined(__GNUC__)
    const std::vector<std::string> endings{"::Box", "::operator()", "::operator bool", "::convert", "::~Box"};
    for (std::size_t i = 0; i < endings.size(); ++i) {
        const auto& name = snapshot.call_tree[start + i].name;
        CHECK(name.rfind("profile_fixture::Box<", 0) == 0);
        CHECK(name.substr(name.size() - endings[i].size()) == endings[i]);
    }
#endif
}

int main() {
    test_invocations_and_snapshot();
    test_lifetime_and_macros();
    test_repeated_paths_and_recursion();
    test_thread_stack_lifetime();
    test_threads_and_live_snapshots();
    test_reports();
    test_report_units();
    test_report_name_width();
    test_member_function_names();
    test_function_name_extraction();
    std::cout << "All profiler tests passed\n";
}
