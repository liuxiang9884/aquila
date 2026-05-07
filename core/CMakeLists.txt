add_library(aquila_core STATIC
    aquila_core.cpp
)

target_include_directories(aquila_core
    PUBLIC ${PROJECT_SOURCE_DIR}
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
add_subdirectory(websocket)
