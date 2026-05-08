add_executable(websocket_probe
    websocket/probe.cpp
)

target_link_libraries(websocket_probe
    PRIVATE
        aquila_core
        CLI11::CLI11
        fmt::fmt-header-only
        magic_enum::magic_enum
)

add_executable(gate_futures_book_ticker_probe
    gate/futures_book_ticker_probe.cpp
)

target_link_libraries(gate_futures_book_ticker_probe
    PRIVATE
        aquila_core
        aquila_gate
        CLI11::CLI11
        fmt::fmt-header-only
        magic_enum::magic_enum
)

add_executable(gate_data_session
    gate/data_session.cpp
)

target_link_libraries(gate_data_session
    PRIVATE
        aquila_config
        aquila_gate
        CLI11::CLI11
        fmt::fmt-header-only
        magic_enum::magic_enum
        nova
)

add_executable(gate_strategy_order
    gate/strategy_order.cpp
)

target_link_libraries(gate_strategy_order
    PRIVATE
        aquila_config
        aquila_gate
        aquila_strategy
        CLI11::CLI11
        fmt::fmt-header-only
        magic_enum::magic_enum
        nova
)

add_executable(gate_order_feedback_session
    gate/order_feedback_session.cpp
)

target_link_libraries(gate_order_feedback_session
    PRIVATE
        aquila_config
        aquila_core
        aquila_gate
        CLI11::CLI11
        fmt::fmt-header-only
        magic_enum::magic_enum
        nova
)

add_executable(binance_data_session
    binance/data_session.cpp
)

target_link_libraries(binance_data_session
    PRIVATE
        aquila_config
        aquila_binance
        CLI11::CLI11
        fmt::fmt-header-only
        magic_enum::magic_enum
        nova
)

add_executable(binance_futures_book_ticker_probe
    binance/futures_book_ticker_probe.cpp
)

target_link_libraries(binance_futures_book_ticker_probe
    PRIVATE
        aquila_core
        aquila_binance
        CLI11::CLI11
        fmt::fmt-header-only
        magic_enum::magic_enum
)

add_executable(book_ticker_shm_reader
    market_data/book_ticker_shm_reader.cpp
)

target_link_libraries(book_ticker_shm_reader
    PRIVATE
        aquila_core
        CLI11::CLI11
        fmt::fmt-header-only
        magic_enum::magic_enum
        nova
)

add_executable(data_reader_probe
    market_data/data_reader_probe.cpp
)

target_link_libraries(data_reader_probe
    PRIVATE
        aquila_config
        aquila_core
        CLI11::CLI11
        magic_enum::magic_enum
        nova
)

add_executable(websocket_latency_compare
    websocket/latency_compare.cpp
)

target_link_libraries(websocket_latency_compare
    PRIVATE
        aquila_core
        absl::flat_hash_map
        CLI11::CLI11
        fmt::fmt-header-only
        magic_enum::magic_enum
        simdjson::simdjson
)
