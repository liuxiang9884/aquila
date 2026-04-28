add_executable(websocket_probe
    websocket_probe.cpp
)

target_link_libraries(websocket_probe
    PRIVATE
        aquila_core
        CLI11::CLI11
        fmt::fmt-header-only
        magic_enum::magic_enum
)

add_executable(websocket_latency_compare
    websocket_latency_compare.cpp
)

target_link_libraries(websocket_latency_compare
    PRIVATE
        aquila_core
        CLI11::CLI11
        fmt::fmt-header-only
        magic_enum::magic_enum
        yyjson::yyjson
)
