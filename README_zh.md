# lite-perf

[English](README.md) | 简体中文

一个无第三方依赖的 C++17 单头文件 profiling 工具。每次埋点函数或代码块调用都会生成一个独立的调用树节点，保存本次耗时及父节点 ID。支持多线程记录，不需要安装性能分析工具。

## 快速使用

把 `include/lite_perf/profiler.hpp` 复制到项目的 include 路径即可：

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

使用 `g++ -std=c++17 -O2 -pthread -Iinclude your_program.cpp -o your_program` 编译。

示意输出（实际时间取决于被测代码）：

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

报告按线程分组，用与 `tree` 命令相同的 `├──`、`└──` 和 `│`展现父子关系及后续分支，并显示占父节点耗时的比例；同级节点保留创建顺序，同名调用分别显示为独立的行。内部以纳秒存储，报告默认以秒显示；纳秒单位并不代表时钟具有纳秒精度。

## 单例与提前停止

所有宏和作用域都记录到 `Profiler::instance()`。`Profiler` 的构造和析构函数为私有，且禁止复制和移动，用户不能创建独立收集器。

```cpp
auto& profiler = lite_perf::Profiler::instance();
{
    LITE_PERF_SCOPE("request");
    // Process the request here.
}
profiler.write_report(std::cout);
```

`LITE_PERF_FUNC()` 仅记录带命名空间和类名的函数名称，例如 `MyClass::run` 或 `my_namespace::MyClass::run`，不包含返回类型、参数列表及末尾的 `const` 等限定符。在 GCC/Clang 下从 `__PRETTY_FUNCTION__` 提取名称，MSVC 下使用 `__FUNCSIG__`；无法识别的签名格式及其他编译器回退到 `__func__`。模板类名保留其模板实参。同一限定名称的重载在汇总报告中合并统计；如需区分，可使用 `LITE_PERF_SCOPE("MyClass::run_int")` 等自定义标签。需要更灵活地控制时间区间时：

```cpp
lite_perf::Scope scope("partial scope");
// Run the work being measured here.
scope.stop(); // End early; later calls or destruction do not submit again.
```

## 获取调用记录

```cpp
auto snapshot = profiler.snapshot();
for (const auto& thread : snapshot.threads) {
    // thread.thread_id identifies the recorded thread.
    for (auto root_id : thread.roots) {
        const auto& root = snapshot.call_tree[root_id];
        // Completed roots are retained too.
    }
}
for (const auto& node : snapshot.call_tree) {
    // node.id, node.parent_id, node.name, node.duration_ns, node.completed
    if (node.completed) {
        // duration_ns is this invocation's inclusive wall time.
    }
}
```

快照独立持有 `call_tree` 调用记录及 `threads` 线程信息，两者在同一个锁内复制，不受后续记录影响。每次调用开始时立即创建节点：活跃调用的 `completed == false`、`duration_ns == 0`；停止后写入本次耗时并标记完成。`completed` 可以区分尚未结束的调用和耗时为零的已完成调用。记录在单例生命周期内持续保留，目前不提供重置接口。

## Scope 调用树

单例为每个线程 ID 保存 vector 调用栈及持久的根节点 ID 列表。作用域停止时移除栈中的 ID，根节点列表在线程退出后仍然保留。`Snapshot::threads` 按各线程首个根节点的创建顺序排列，每个线程内的根节点也保留创建顺序。复用的 `std::thread::id` 会归入同一个报告分组。可以按线程输出调用树：

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

`write_report(out, bool verbose = false, TimeUnit unit = TimeUnit::Seconds)` 按线程输出带 `tree` 风格 Unicode 分支的调用树。默认每行包含名称、本次耗时、占父节点耗时的比例及完成状态；传入 `true` 时额外输出 `ID` 和 `Parent ID` 两列。`Snapshot::write_report` 同样支持此参数。

两种报告的 Name 列均会根据名称长度（含转义后的字符）自动扩展；调用树报告还会计入层级缩进，使后续各列保持对齐。

- 占比为 `100 * child.duration_ns / parent.duration_ns`，保留两位小数。根节点、未完成调用、父节点未完成或父节点耗时为零时显示 `N/A`；未完成调用的耗时同样显示 `N/A`。
- 如果父作用域显式提前停止，子调用完成后的占比可能超过 100%，报告不会截断此比例。
- 每次调用均有独立节点，同名、重复路径、递归、不同线程的调用分别保留。每个线程内的根节点与同级子节点保留创建顺序。
- 父节点取同线程中最近的活跃作用域，新线程不会继承上下文；本库不提供跨线程任务上下文传播。
- 父作用域提前停止时，已创建子节点的父 ID 不变；后续作用域挂到最近仍活跃的节点下。非栈顶停止通过删除 vector 中对应 ID 实现。
- ID 从 0 开始，与 `call_tree` 下标一致。根节点满足 `parent_id == id`；这里只记录埋点作用域的嵌套关系。
- 所有调用记录及根节点列表保留到单例析构，内存随调用总次数增长。

## 报告时间单位

两种报告均支持 `lite_perf::TimeUnit`：`Seconds`（秒，`s`，默认）、`Milliseconds`（毫秒，`ms`）、`Microseconds`（微秒，`us`）、`Nanoseconds`（纳秒，`ns`）。标题和表头随所选单位变化，数值保留六位小数；父节点占比不受时间单位影响。内部的 `duration_ns` 始终以纳秒保存。

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

## 汇总报告

```cpp
profiler.write_report(std::cout);         // Call trees grouped by thread; hide IDs.
profiler.write_report(std::cout, true);   // Also show ID and Parent ID columns.
profiler.write_summary_report(std::cout); // Same-name statistics across all threads.

// Both reports can also use the same saved snapshot:
auto saved = profiler.snapshot();
saved.write_report(std::cout);
saved.write_summary_report(std::cout);
```

汇总报告跨线程、跨路径合并同名 Scope，按已完成调用的总耗时从大到小排序，总耗时相同时按名称排序。`Calls`、`Total (s)`、`Min (s)`、`Max (s)`、`Mean (s)` **只统计已完成调用**；`Incomplete` 单独列出未完成数量，其耗时不参与计算。某个名称只有未完成调用时，次数和总耗时为零，最小、最大及平均耗时显示 `N/A`。已完成的零耗时调用正常参与统计。两种报告均保留输出流原来的格式与精度设置。

## 关闭埋点

编译时添加 `-DLITE_PERF_ENABLED=0`，或在首次 include 前定义：

```cpp
#define LITE_PERF_ENABLED 0
#include <lite_perf/profiler.hpp>
```

此时 `LITE_PERF_SCOPE`、`LITE_PERF_FUNC` 都为空操作，参数不会求值，不会构造计时器或访问收集器。直接构造 `Scope` 仍然有效。宏开关请在整个项目中保持一致，避免包含埋点的 inline 函数定义不一致。

## 计时语义与开销

- 使用 `std::chrono::steady_clock` 测量实际经过的时间（wall time），包含睡眠、I/O 和锁等待，不能据此直接推导 CPU 使用率。纳秒单位不代表时钟具有纳秒精度。
- 每个节点记录本次调用的 inclusive time。通常父作用域包含子作用域；显式提前停止父作用域会立即结束其计时区间。嵌套和并行区间可能重叠，不能将所有节点耗时相加作为程序总运行时间；这里不计算 exclusive time。
- `snapshot()` 和报告方法支持并发调用。登记、结束、调用栈维护共用一个互斥锁。每个 `Scope` 必须在同一个线程构造、停止和销毁；共享输出流由调用方同步。
- 请在计时作用域结束、工作线程 join 后输出最终报告。宏是局部变量声明，应放在花括号内。
- 标签复制到节点中。登记时的内存分配、标签复制和加锁等待不计入该作用域耗时；子作用域的维护开销会计入父作用域耗时。结束时等待提交锁的时间不计入本次耗时。
- `Scope::stop()` 和析构不会抛异常，同一次调用只提交一次。提交异常会被捕获以保证析构不抛出，失败的调用可能仍显示为活跃；构造、快照和输出仍可能因内存分配或流错误抛出异常。登记失败会回滚节点及调用栈条目。
- 单例直接持有所有调用记录。请在单例析构前 join 工作线程并结束所有作用域，避免在全局对象析构期间首次使用单例。

## 构建示例与测试

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/profiler_example
./build/profiler_multilevel_example
./build/profiler_namespaced_class_example
```

`examples/multi_level.cpp` 展示四层函数调用：`run_batch → process_item → load_input → read_input`，并输出按线程分组的调用树及汇总报告。调用树保留六个独立的 `validate_input` 节点，汇总报告则合并为一行，完成调用次数为六次。

`examples/namespaced_class.cpp` 展示嵌套命名空间、类、const 成员函数及静态成员函数。报告中的名称如 `demo::pipeline::Processor::process`，保留命名空间和类名，不包含返回类型和参数列表。调用树和汇总报告均使用毫秒。

在已有 CMake 项目中使用：

```cmake
set(LITE_PERF_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LITE_PERF_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(path/to/lite-perf)
target_link_libraries(your_target PRIVATE lite_perf::lite_perf)
```

测试覆盖逐次调用耗时、同名与重复路径、RAII、异常/提前返回、重复停止、标签生命周期、递归、vector 扩容、非栈顶停止、线程隔离、并发快照、跨编译单元嵌套、持久线程根节点、父节点耗时占比、仅统计完成调用的汇总报告及关闭埋点后的参数不求值。
