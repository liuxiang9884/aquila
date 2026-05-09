add_executable(core_base_structures_benchmark
    base_structures_benchmark.cpp
)

target_link_libraries(core_base_structures_benchmark
    PRIVATE
        aquila_core
        benchmark::benchmark_main
)
