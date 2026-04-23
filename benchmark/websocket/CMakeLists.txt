add_executable(prepared_write_benchmark
    prepared_write_benchmark.cpp
)

target_link_libraries(prepared_write_benchmark
    PRIVATE aquila_gate_ws_core
)

add_executable(frame_codec_benchmark
    frame_codec_benchmark.cpp
)

target_link_libraries(frame_codec_benchmark
    PRIVATE aquila_gate_ws_core
)

add_executable(active_spin_benchmark
    active_spin_benchmark.cpp
)

target_link_libraries(active_spin_benchmark
    PRIVATE aquila_gate_ws_core
)
