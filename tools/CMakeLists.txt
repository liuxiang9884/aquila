add_executable(aquila_hello_world
    hello_world.cpp
)

add_executable(websocket_probe
    websocket_probe.cpp
)

target_link_libraries(websocket_probe
    PRIVATE aquila_websocket_core CLI11::CLI11
)
