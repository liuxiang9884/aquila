add_library(aquila_core STATIC
    aquila_core.cpp
)

target_include_directories(aquila_core
    PUBLIC ${PROJECT_SOURCE_DIR}
)

target_compile_definitions(aquila_core
    PUBLIC
        AQUILA_ORDER_ACK_DIAG_LEVEL=${AQUILA_ORDER_ACK_DIAG_LEVEL}
        AQUILA_DATA_SESSION_DIAG_LEVEL=${AQUILA_DATA_SESSION_DIAG_LEVEL}
        AQUILA_BOOK_TICKER_FUSION_METADATA_ENABLED=${AQUILA_BOOK_TICKER_FUSION_METADATA_ENABLED}
        $<$<BOOL:${AQUILA_ENABLE_SOCKET_TIMESTAMPING_ATTRIBUTION}>:AQUILA_ENABLE_SOCKET_TIMESTAMPING_ATTRIBUTION=1>
        $<$<NOT:$<BOOL:${AQUILA_ENABLE_SOCKET_TIMESTAMPING_ATTRIBUTION}>>:AQUILA_ENABLE_SOCKET_TIMESTAMPING_ATTRIBUTION=0>
)

target_link_libraries(aquila_core
    PUBLIC
        Threads::Threads
        fmt::fmt-header-only
        FastFloat::fast_float
        OpenSSL::SSL
        OpenSSL::Crypto
        absl::flat_hash_map
)

add_subdirectory(config)
add_subdirectory(market_data)
add_subdirectory(websocket)
