add_executable(aquila_hello_world
    hello_world.cpp
)

target_link_libraries(aquila_hello_world
    PRIVATE
        fmt::fmt-header-only
)

add_executable(websocket_probe
    websocket_probe.cpp
)

target_link_libraries(websocket_probe
    PRIVATE
        aquila_websocket_core
        CLI11::CLI11
        fmt::fmt-header-only
        magic_enum::magic_enum
)
