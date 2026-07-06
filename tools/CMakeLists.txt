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

add_executable(generate_time_series_data
    benchmark/generate_time_series.cpp
)

target_link_libraries(generate_time_series_data
    PRIVATE
        aquila_core
        CLI11::CLI11
        fmt::fmt-header-only
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

add_executable(gate_order_session_failure_probe
    gate/order_session_failure_probe.cpp
)

target_link_libraries(gate_order_session_failure_probe
    PRIVATE
        aquila_config
        aquila_gate
        CLI11::CLI11
        fmt::fmt-header-only
        magic_enum::magic_enum
        nova
)

add_executable(gate_order_gateway
    gate/gate_order_gateway.cpp
)

target_link_libraries(gate_order_gateway
    PRIVATE
        aquila_config
        aquila_core
        aquila_gate
        CLI11::CLI11
        fmt::fmt-header-only
        nova
        PkgConfig::tomlplusplus
)

add_executable(fill_probe_strategy
    gate/fill_probe/config.cpp
    gate/fill_probe/csv_writer.cpp
    gate/fill_probe/state_machine.cpp
    gate/fill_probe/main.cpp
)

target_link_libraries(fill_probe_strategy
    PRIVATE
        aquila_config
        aquila_core
        aquila_gate
        CLI11::CLI11
        fmt::fmt-header-only
        magic_enum::magic_enum
        nova
        PkgConfig::tomlplusplus
)

add_executable(gate_order_session_rtt_probe
    gate/order_session_rtt_probe/connection_plan.cpp
    gate/order_session_rtt_probe/config.cpp
    gate/order_session_rtt_probe/sample_csv_writer.cpp
    gate/order_session_rtt_probe/main.cpp
)

target_link_libraries(gate_order_session_rtt_probe
    PRIVATE
        aquila_config
        aquila_core
        aquila_gate
        absl::flat_hash_set
        CLI11::CLI11
        fmt::fmt-header-only
        magic_enum::magic_enum
        nova
        PkgConfig::tomlplusplus
        quill::quill
        unofficial::vincentlaucsb-csv-parser::csv
)

add_executable(gate_demo_strategy
    gate/demo_strategy.cpp
)

target_link_libraries(gate_demo_strategy
    PRIVATE
        aquila_config
        aquila_gate
        aquila_strategy
        CLI11::CLI11
        fmt::fmt-header-only
        magic_enum::magic_enum
        nova
)

add_executable(lead_lag_replay
    lead_lag/replay.cpp
    lead_lag/lag_vol_guard_audit.cpp
)

target_link_libraries(lead_lag_replay
    PRIVATE
        aquila_config
        aquila_core
        aquila_lead_lag_diagnostics
        aquila_strategy
        CLI11::CLI11
        fmt::fmt-header-only
        magic_enum::magic_enum
        nova
        quill::quill
)

add_executable(lead_lag_strategy
    lead_lag/live_strategy.cpp
)

target_link_libraries(lead_lag_strategy
    PRIVATE
        aquila_config
        aquila_core
        aquila_gate
        aquila_lead_lag_diagnostics
        aquila_strategy
        CLI11::CLI11
        fmt::fmt-header-only
        magic_enum::magic_enum
        nova
        quill::quill
)

add_executable(lead_lag_freshness_preflight
    lead_lag/freshness_preflight_main.cpp
    lead_lag/freshness_preflight.cpp
)

target_link_libraries(lead_lag_freshness_preflight
    PRIVATE
        aquila_config
        aquila_core
        CLI11::CLI11
        fmt::fmt-header-only
        magic_enum::magic_enum
        nova
        PkgConfig::tomlplusplus
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

add_executable(tardis_book_ticker_to_binary
    tardis/book_ticker_to_binary.cpp
)

target_link_libraries(tardis_book_ticker_to_binary
    PRIVATE
        aquila_config
        aquila_core
        CLI11::CLI11
        fmt::fmt-header-only
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

add_executable(gate_book_ticker_fusion
    market_data/book_ticker_fusion.cpp
    market_data/book_ticker_fusion_cli.cpp
)

target_link_libraries(gate_book_ticker_fusion
    PRIVATE
        aquila_config
        aquila_core
        CLI11::CLI11
        fmt::fmt-header-only
        nova
)

add_executable(binance_book_ticker_fusion
    market_data/binance_book_ticker_fusion.cpp
    market_data/book_ticker_fusion_cli.cpp
)

target_link_libraries(binance_book_ticker_fusion
    PRIVATE
        aquila_config
        aquila_core
        CLI11::CLI11
        fmt::fmt-header-only
        nova
)

add_executable(gate_trade_fusion
    market_data/trade_fusion.cpp
    market_data/trade_fusion_cli.cpp
)

target_link_libraries(gate_trade_fusion
    PRIVATE
        aquila_config
        aquila_core
        CLI11::CLI11
        fmt::fmt-header-only
        nova
)

add_executable(binance_trade_fusion
    market_data/binance_trade_fusion.cpp
    market_data/trade_fusion_cli.cpp
)

target_link_libraries(binance_trade_fusion
    PRIVATE
        aquila_config
        aquila_core
        CLI11::CLI11
        fmt::fmt-header-only
        nova
)

add_executable(gate_data_fusion
    gate/gate_data_fusion.cpp
    gate/gate_data_fusion_config.cpp
)

target_link_libraries(gate_data_fusion
    PRIVATE
        aquila_config
        aquila_core
        aquila_gate
        CLI11::CLI11
        fmt::fmt-header-only
        nova
        PkgConfig::tomlplusplus
)

add_executable(binance_data_fusion
    binance/binance_data_fusion.cpp
    binance/binance_data_fusion_config.cpp
)

target_link_libraries(binance_data_fusion
    PRIVATE
        aquila_binance
        aquila_config
        aquila_core
        CLI11::CLI11
        fmt::fmt-header-only
        nova
        PkgConfig::tomlplusplus
)

target_compile_options(binance_data_fusion
    PRIVATE
        $<$<CXX_COMPILER_ID:GNU>:-Wno-stringop-overflow>
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

add_executable(data_reader_recorder
    market_data/data_reader_recorder.cpp
    market_data/data_reader_recorder_config.cpp
)

target_link_libraries(data_reader_recorder
    PRIVATE
        aquila_config
        aquila_core
        CLI11::CLI11
        fmt::fmt-header-only
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
