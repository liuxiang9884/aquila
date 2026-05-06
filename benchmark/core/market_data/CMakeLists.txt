add_executable(data_shm_benchmark
    data_shm_benchmark.cpp
)

target_link_libraries(data_shm_benchmark
    PRIVATE
        aquila_core
        benchmark::benchmark_main
        fmt::fmt-header-only
        nova
)
