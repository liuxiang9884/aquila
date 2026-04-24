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

add_executable(session_write_path_benchmark
    session_write_path_benchmark.cpp
)

target_link_libraries(session_write_path_benchmark
    PRIVATE aquila_gate_ws_core
)

add_executable(session_read_path_benchmark
    session_read_path_benchmark.cpp
)

target_link_libraries(session_read_path_benchmark
    PRIVATE aquila_gate_ws_core
)

add_executable(runtime_loopback_benchmark
    runtime_loopback_benchmark.cpp
)

target_link_libraries(runtime_loopback_benchmark
    PRIVATE aquila_gate_ws_core
)
