add_library(aquila_websocket_core INTERFACE)

target_include_directories(aquila_websocket_core
    INTERFACE ${PROJECT_SOURCE_DIR}
)

target_link_libraries(aquila_websocket_core
    INTERFACE
        Threads::Threads
        fmt::fmt-header-only
        OpenSSL::SSL
        OpenSSL::Crypto
)
