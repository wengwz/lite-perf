cmake_minimum_required(VERSION 3.14)
project(lite-perf VERSION 0.1.0 LANGUAGES CXX)

find_package(Threads REQUIRED)

# This header-only library propagates include paths and dependencies through an INTERFACE target.
add_library(lite_perf INTERFACE)
add_library(lite_perf::lite_perf ALIAS lite_perf)
target_include_directories(lite_perf INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(lite_perf INTERFACE cxx_std_17)
target_link_libraries(lite_perf INTERFACE Threads::Threads)

option(LITE_PERF_BUILD_EXAMPLES "Build the profiling examples" ON)
option(LITE_PERF_BUILD_TESTS "Build the tests" ON)

if(LITE_PERF_BUILD_EXAMPLES)
    add_executable(profiler_example examples/basic.cpp)
    target_link_libraries(profiler_example PRIVATE lite_perf::lite_perf)

    add_executable(profiler_multilevel_example examples/multi_level.cpp)
    target_link_libraries(profiler_multilevel_example PRIVATE lite_perf::lite_perf)

    add_executable(profiler_namespaced_class_example examples/namespaced_class.cpp)
    target_link_libraries(profiler_namespaced_class_example PRIVATE lite_perf::lite_perf)
endif()

if(LITE_PERF_BUILD_TESTS)
    enable_testing()
    add_executable(profiler_tests tests/profiler_test.cpp tests/other_translation_unit.cpp)
    target_link_libraries(profiler_tests PRIVATE lite_perf::lite_perf)
    add_test(NAME profiler_tests COMMAND profiler_tests)

    add_executable(profiler_disabled_tests tests/disabled_test.cpp)
    target_link_libraries(profiler_disabled_tests PRIVATE lite_perf::lite_perf)
    # Compile disabled instrumentation separately to verify that macro arguments are not evaluated.
    target_compile_definitions(profiler_disabled_tests PRIVATE LITE_PERF_ENABLED=0)
    add_test(NAME profiler_disabled_tests COMMAND profiler_disabled_tests)
endif()
