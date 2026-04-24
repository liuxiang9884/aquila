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
        OpenSSL::SSL
        OpenSSL::Crypto
)

add_subdirectory(websocket)
