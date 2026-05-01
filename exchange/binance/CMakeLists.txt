add_library(aquila_binance INTERFACE)

target_include_directories(aquila_binance
    INTERFACE
        ${PROJECT_SOURCE_DIR}
)

target_link_libraries(aquila_binance
    INTERFACE
        fmt::fmt-header-only
        simdjson::simdjson
        yyjson::yyjson
        FastFloat::fast_float
        absl::flat_hash_map
)
