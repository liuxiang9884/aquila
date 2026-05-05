add_library(aquila_binance STATIC
    market_data/data_session_config.cpp
    market_data/data_session_config.h
)

target_include_directories(aquila_binance
    PUBLIC
        ${PROJECT_SOURCE_DIR}
)

target_link_libraries(aquila_binance
    PUBLIC
        aquila_config
        fmt::fmt-header-only
        simdjson::simdjson
        FastFloat::fast_float
        absl::flat_hash_map
    PRIVATE
        nova
)
