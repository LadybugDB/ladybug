include_directories(
        ./include
        ${PROJECT_SOURCE_DIR}/test/include/test_helper)

add_executable(lbug_benchmark
        benchmark.cpp
        benchmark_parser.cpp
        benchmark_runner.cpp
        main.cpp)

target_link_libraries(lbug_benchmark lbug test_helper)
