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

add_executable(gate_futures_book_ticker_probe
    gate_futures_book_ticker_probe.cpp
)

target_link_libraries(gate_futures_book_ticker_probe
    PRIVATE
        aquila_core
        aquila_gate
        CLI11::CLI11
        fmt::fmt-header-only
        magic_enum::magic_enum
)

add_executable(binance_futures_book_ticker_probe
    binance_futures_book_ticker_probe.cpp
)

target_link_libraries(binance_futures_book_ticker_probe
    PRIVATE
        aquila_core
        aquila_binance
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
        simdjson::simdjson
)
