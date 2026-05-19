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

add_executable(data_reader_benchmark
    data_reader_benchmark.cpp
)

target_link_libraries(data_reader_benchmark
    PRIVATE
        aquila_config
        aquila_core
        benchmark::benchmark_main
        fmt::fmt-header-only
        nova
)
