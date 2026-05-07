add_library(aquila_gate STATIC
    market_data/data_session_config.cpp
    market_data/data_session_config.h
    trading/order_request_encoder.h
    trading/order_signature.cpp
    trading/order_signature.h
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
        OpenSSL::Crypto
)
