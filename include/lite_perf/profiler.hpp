#ifndef LITE_PERF_PROFILER_HPP
#define LITE_PERF_PROFILER_HPP

/**
 * @file profiler.hpp
 * @brief Dependency-free C++17 instrumentation timers and per-invocation call trees.
 *
 * LITE_PERF_FUNC() times the current function; LITE_PERF_SCOPE("label") times its enclosing block.
 * Timers submit samples on destruction; print final reports after scopes end and workers join.
 * Measurements are elapsed wall time, including I/O, sleep, and lock waits, rather than CPU time.
 */

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <map>
#include <mutex>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lite_perf {

// steady_clock is monotonic and unaffected by system clock adjustments, making it suitable for intervals.
// high_resolution_clock may alias a non-monotonic clock and offers no equivalent portable guarantee.
// Nanoseconds are the storage unit, not a guarantee of the underlying clock resolution.
using Clock = std::chrono::steady_clock;

/// Display units only; recorded durations always remain in nanoseconds.
enum class TimeUnit { Seconds, Milliseconds, Microseconds, Nanoseconds };

/// One scope invocation. IDs are zero-based; parent_id == id denotes a root.
struct CallTreeNode {
    std::size_t id = 0;
    std::size_t parent_id = 0;
    std::string name;              ///< Owned label, independent of the caller's string lifetime.
    std::uint64_t duration_ns = 0;  ///< This invocation's inclusive wall time; set when it stops.
    bool completed = false;        ///< Distinguishes active calls from completed zero-duration calls.
};

/// Roots recorded for one std::thread::id, retained after its scopes and thread exit.
struct ThreadSnapshot {
    std::thread::id thread_id;
    std::vector<std::size_t> roots;
};

/// Owned, consistent copies of invocation records and their thread roots.
struct Snapshot {
    // IDs index this vector. Each invocation remains separate, in creation order.
    std::vector<CallTreeNode> call_tree;
    // Threads are ordered by their first root's creation ID; roots stay in creation order.
    std::vector<ThreadSnapshot> threads;

    /// Render the invocation hierarchy grouped by thread, with inclusive time and parent percentage.
    /// Ratios are unavailable for roots, active calls, or active/zero-duration parents.
    /// unit defaults to seconds; verbose includes ID and Parent ID columns. Tree-style Unicode branches show sibling relationships.
    /// Output uses a private buffer to preserve the caller's stream format.
    void write_report(std::ostream& out, bool verbose = false, TimeUnit unit = TimeUnit::Seconds) const {
        std::vector<std::vector<std::size_t>> children(call_tree.size());
        for (const auto& node : call_tree) {
            if (node.parent_id != node.id) {
                children[node.parent_id].push_back(node.id);
            }
        }
        std::ostringstream report;
        const auto format = time_format(unit);
        report << "lite-perf call tree (inclusive wall time, " << format.name << ")\n";
        // Iterative traversal avoids using the C++ stack for deeply nested calls.
        struct PendingNode {
            std::size_t id;
            std::size_t depth;
            bool last;
        };
        std::vector<PendingNode> pending;
        std::vector<bool> continuation;
        for (const auto& thread : threads) {
            report << "Thread " << thread.thread_id << '\n';
            report << std::left << std::setw(32) << "Name" << std::right;
            if (verbose) {
                report << std::setw(12) << "ID" << std::setw(12) << "Parent ID";
            }
            report << ' ' << std::setw(16) << (std::string("Duration (") + format.symbol + ")")
                   << ' ' << std::setw(14) << "Parent (%)"
                   << ' ' << std::setw(12) << "Status" << '\n'
                   << std::fixed << std::setprecision(6);
            for (std::size_t i = thread.roots.size(); i > 0; --i) {
                pending.push_back({thread.roots[i - 1], 0, i == thread.roots.size()});
            }
            while (!pending.empty()) {
                const auto item = pending.back();
                pending.pop_back();
                const auto& node = call_tree[item.id];
                continuation.resize(item.depth);
                std::string prefix;
                for (bool more : continuation) { prefix += more ? "│   " : "    "; }
                prefix += item.last ? "└── " : "├── ";
                continuation.push_back(!item.last);
                // setw counts UTF-8 bytes; compensate for the branch glyphs' display width.
                const auto name_width = 32 + prefix.size() - 4 * (item.depth + 1);
                report << std::left << std::setw(static_cast<int>(name_width)) << (prefix + quote(node.name))
                       << ' ' << std::right;
                if (verbose) {
                    report << std::setw(11) << node.id << std::setw(12) << node.parent_id;
                }
                report << ' ' << std::setw(16) << duration(node.duration_ns, unit, node.completed)
                       << ' ' << std::setw(14) << parent_percentage(node)
                       << ' ' << std::setw(12) << (node.completed ? "completed" : "active") << '\n';
                const auto& descendants = children[node.id];
                for (std::size_t i = descendants.size(); i > 0; --i) {
                    pending.push_back({descendants[i - 1], item.depth + 1, i == descendants.size()});
                }
            }
        }
        out << report.str();
    }

    /// Aggregate completed calls by label across all threads; count unfinished calls separately.
    void write_summary_report(std::ostream& out, TimeUnit unit = TimeUnit::Seconds) const {
        struct Summary {
            std::uint64_t count = 0;
            std::uint64_t total_ns = 0;
            std::uint64_t min_ns = 0;
            std::uint64_t max_ns = 0;
            std::uint64_t incomplete = 0;
        };
        std::map<std::string, Summary> summaries;
        for (const auto& node : call_tree) {
            auto& summary = summaries[node.name];
            if (!node.completed) {
                ++summary.incomplete;
                continue;
            }
            summary.min_ns = summary.count == 0 ? node.duration_ns : std::min(summary.min_ns, node.duration_ns);
            summary.max_ns = std::max(summary.max_ns, node.duration_ns);
            summary.total_ns += node.duration_ns;
            ++summary.count;
        }
        std::ostringstream report;
        const auto format = time_format(unit);
        report << "lite-perf summary (completed calls, " << format.name << ")\n"
               << std::left << std::setw(32) << "Name"
               << std::right << std::setw(12) << "Calls"
               << ' ' << std::setw(16) << (std::string("Total (") + format.symbol + ")")
               << ' ' << std::setw(16) << (std::string("Min (") + format.symbol + ")")
               << ' ' << std::setw(16) << (std::string("Max (") + format.symbol + ")")
               << ' ' << std::setw(16) << (std::string("Mean (") + format.symbol + ")")
               << ' ' << std::setw(14) << "Incomplete" << '\n';
        for (const auto& entry : summaries) {
            const auto& stats = entry.second;
            const auto mean = stats.count == 0 ? 0.0 : static_cast<double>(stats.total_ns) / stats.count;
            report << std::left << std::setw(32) << quote(entry.first)
                   << ' ' << std::right << std::setw(11) << stats.count
                   << ' ' << std::setw(16) << duration(stats.total_ns, unit)
                   << ' ' << std::setw(16) << duration(stats.min_ns, unit, stats.count != 0)
                   << ' ' << std::setw(16) << duration(stats.max_ns, unit, stats.count != 0)
                   << ' ' << std::setw(16) << duration(mean, unit, stats.count != 0)
                   << ' ' << std::setw(14) << stats.incomplete << '\n';
        }
        out << report.str();
    }

private:
    static std::string quote(std::string_view name) {
        std::string label = "\"";
        for (char c : name) {
            switch (c) {
            case '\n': label += "\\n"; break;
            case '\r': label += "\\r"; break;
            case '\t': label += "\\t"; break;
            case '\\': label += "\\\\"; break;
            case '"': label += "\\\""; break;
            default: label += c;
            }
        }
        return label + '"';
    }

    struct TimeFormat {
        const char* symbol;
        const char* name;
        double nanoseconds_per_unit;
    };

    static TimeFormat time_format(TimeUnit unit) {
        switch (unit) {
        case TimeUnit::Milliseconds: return {"ms", "milliseconds", 1e6};
        case TimeUnit::Microseconds: return {"us", "microseconds", 1e3};
        case TimeUnit::Nanoseconds: return {"ns", "nanoseconds", 1.0};
        case TimeUnit::Seconds: return {"s", "seconds", 1e9};
        }
        return {"s", "seconds", 1e9};
    }

    static std::string duration(double ns, TimeUnit unit, bool available = true) {
        if (!available) { return "N/A"; }
        std::ostringstream value;
        value << std::fixed << std::setprecision(6) << ns / time_format(unit).nanoseconds_per_unit;
        return value.str();
    }

    std::string parent_percentage(const CallTreeNode& node) const {
        if (node.parent_id == node.id || !node.completed) { return "N/A"; }
        const auto& parent = call_tree[node.parent_id];
        if (!parent.completed || parent.duration_ns == 0) { return "N/A"; }
        std::ostringstream value;
        value << std::fixed << std::setprecision(2)
              << 100.0 * static_cast<double>(node.duration_ns) / parent.duration_ns << '%';
        return value.str();
    }
};

class Scope;

/**
 * @brief Singleton that records each scope invocation and its parent across threads.
 *
 * snapshot()/write_report() may run concurrently on the same instance.
 * Callers must synchronize shared output streams and join workers before process exit.
 * Each invocation is retained until singleton destruction; memory grows with the number of calls.
 */
class Profiler {
public:
    Profiler(const Profiler&) = delete;
    Profiler& operator=(const Profiler&) = delete;
    Profiler(Profiler&&) = delete;
    Profiler& operator=(Profiler&&) = delete;

    /// The only collector; C++ guarantees thread-safe static initialization.
    /// Join workers before process exit to avoid accessing the singleton after destruction.
    static Profiler& instance() {
        static Profiler profiler;
        return profiler;
    }

    /// Copy invocations and persistent per-thread roots under the same lock.
    Snapshot snapshot() const {
        std::lock_guard<std::mutex> lock(mutex);
        Snapshot result;
        result.call_tree = call_tree;
        result.threads.reserve(threads_.size());
        for (const auto& thread : threads_) {
            result.threads.push_back({thread.first, thread.second.roots});
        }
        std::sort(result.threads.begin(), result.threads.end(), [](const auto& a, const auto& b) {
            return a.roots.front() < b.roots.front();
        });
        return result;
    }

    /// Render outside the collector lock; callers synchronize shared output streams.
    void write_report(std::ostream& out, bool verbose = false, TimeUnit unit = TimeUnit::Seconds) const {
        snapshot().write_report(out, verbose, unit);
    }
    void write_summary_report(std::ostream& out, TimeUnit unit = TimeUnit::Seconds) const {
        snapshot().write_summary_report(out, unit);
    }

private:
    friend class Scope;
    Profiler() = default;
    ~Profiler() = default;

    // The same mutex protects all invocation records and thread stacks.
    mutable std::mutex mutex;
    struct ThreadData {
        std::vector<std::size_t> call_stack;
        std::vector<std::size_t> roots;
    };
    std::unordered_map<std::thread::id, ThreadData> threads_;
    std::vector<CallTreeNode> call_tree;

    std::size_t begin_scope(std::string_view name) {
        std::lock_guard<std::mutex> lock(mutex);
        auto result = threads_.try_emplace(std::this_thread::get_id());
        auto& thread = result.first->second;
        auto& stack = thread.call_stack;
        const auto node_id = call_tree.size();
        const bool root = stack.empty();
        const auto previous_depth = stack.size();
        const auto previous_roots = thread.roots.size();
        try {
            CallTreeNode node{node_id, root ? node_id : stack.back(), std::string(name)};
            stack.push_back(node_id);
            if (root) { thread.roots.push_back(node_id); }
            call_tree.push_back(std::move(node));
        } catch (...) {
            // Roll back only this registration; previously recorded roots remain intact.
            if (stack.size() > previous_depth) { stack.pop_back(); }
            if (thread.roots.size() > previous_roots) { thread.roots.pop_back(); }
            if (result.second) { threads_.erase(result.first); }
            throw;
        }
        return node_id;
    }

    // Detach the invocation from its thread stack and record its own duration under one lock.
    void end_scope(std::uint64_t ns, std::size_t node_id) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto stack = threads_.find(std::this_thread::get_id());
        if (stack != threads_.end()) {
            auto& ids = stack->second.call_stack;
            if (ids.back() == node_id) {
                ids.pop_back();
            } else {
                const auto position = std::find(ids.begin(), ids.end(), node_id);
                if (position != ids.end()) {
                    ids.erase(position);
                }
            }
        }
        auto& node = call_tree[node_id];
        node.duration_ns = ns;
        node.completed = true;
    }
};

/**
 * @brief RAII timer that submits once on stop() or destruction, including during exception unwinding.
 *
 * Timings are inclusive: parent scopes include child durations without subtracting them.
 * All scopes submit to the singleton Profiler, including across threads.
 * Construct, stop, and destroy each Scope on the same thread; a single Scope is not thread-safe.
 * Copying and moving are disabled to tie each interval to one unambiguous object lifetime.
 */
class Scope {
public:
    /// Copy the label into a new invocation before starting; construction may throw on allocation.
    /// Label copying and registration are excluded from timing but included in any enclosing scope.
    explicit Scope(std::string_view name)
        : node_id_(Profiler::instance().begin_scope(name)), start_() {
        start_ = Clock::now();
    }

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    Scope(Scope&&) = delete;
    Scope& operator=(Scope&&) = delete;

    ~Scope() noexcept { stop(); }

    /// End early and submit at most once; later calls and destruction become no-ops.
    /// Never throws, including during stack unwinding; completion errors are suppressed.
    void stop() noexcept {
        if (stopped_) {
            return;
        }
        // Read the end time before acquiring the collector lock, excluding submission lock waits.
        const auto end = Clock::now();
        stopped_ = true;
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start_).count();
        try {
            Profiler::instance().end_scope(static_cast<std::uint64_t>(ns), node_id_);
        } catch (...) {
            // Avoid a second exception during stack unwinding, which would call std::terminate.
        }
    }

private:
    std::size_t node_id_;
    Clock::time_point start_;
    bool stopped_ = false;
};

} // namespace lite_perf

#ifndef LITE_PERF_ENABLED
// Define as 0 before the first include to disable macros; explicit class APIs remain available.
// Keep this consistent across translation units to avoid conflicting instrumented inline definitions.
#define LITE_PERF_ENABLED 1
#endif

// Two expansion levels resolve __COUNTER__/__LINE__ before token concatenation.
#define LITE_PERF_DETAIL_JOIN_IMPL(a, b) a##b
#define LITE_PERF_DETAIL_JOIN(a, b) LITE_PERF_DETAIL_JOIN_IMPL(a, b)
#ifdef __COUNTER__
// Allow multiple scopes on one line; compilers without this extension fall back to line numbers.
#define LITE_PERF_DETAIL_UNIQUE __COUNTER__
#else
#define LITE_PERF_DETAIL_UNIQUE __LINE__
#endif

#if LITE_PERF_ENABLED
// Expand to a local declaration whose lifetime extends to the end of the enclosing block.
// Use braces in conditionals/loops; a do/while wrapper would destroy the timer immediately.
/// Time the current scope with the given label using the singleton.
#define LITE_PERF_SCOPE(name) \
    ::lite_perf::Scope LITE_PERF_DETAIL_JOIN(lite_perf_scope_, LITE_PERF_DETAIL_UNIQUE){name}
/// Use the current function name; use LITE_PERF_SCOPE to distinguish overloads or namesakes.
#define LITE_PERF_FUNC() LITE_PERF_SCOPE(__func__)
#else
// Remove arguments during preprocessing: do not evaluate labels or read the clock.
#define LITE_PERF_SCOPE(name) ((void)0)
#define LITE_PERF_FUNC() ((void)0)
#endif

#endif // LITE_PERF_PROFILER_HPP
