add_library(aquila_bitget STATIC
    market_data/data_session_config.cpp
    market_data/data_session_config.h
)

target_include_directories(aquila_bitget
    PUBLIC
        ${PROJECT_SOURCE_DIR}
)

target_link_libraries(aquila_bitget
    PUBLIC
        aquila_config
        aquila_core
        fmt::fmt-header-only
        simdjson::simdjson
        absl::flat_hash_map
        nova
)
