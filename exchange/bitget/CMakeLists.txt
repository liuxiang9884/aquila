add_library(aquila_bitget STATIC
    market_data/data_session_config.cpp
    market_data/data_session_config.h
    trading/order_session_config.cpp
    trading/order_session_config.h
    trading/order_feedback_session_config.cpp
    trading/order_feedback_session_config.h
    trading/order_session_runtime_adapter.h
    trading/order_request_encoder.h
    trading/order_signature.cpp
    trading/order_signature.h
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
    PRIVATE
        OpenSSL::Crypto
)
