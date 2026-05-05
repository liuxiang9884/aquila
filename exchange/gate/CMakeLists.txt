add_library(aquila_gate STATIC
    market_data/data_session_config.cpp
    market_data/data_session_config.h
)

target_include_directories(aquila_gate
    PUBLIC
        ${PROJECT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/exchange/gate/sbe/generated
)

target_link_libraries(aquila_gate
    PUBLIC
        aquila_config
        fmt::fmt-header-only
        simdjson::simdjson
        FastFloat::fast_float
        absl::flat_hash_map
    PRIVATE
        nova
)
