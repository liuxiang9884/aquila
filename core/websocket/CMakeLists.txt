add_library(aquila_gate_ws_core INTERFACE)

target_include_directories(aquila_gate_ws_core
    INTERFACE ${PROJECT_SOURCE_DIR}
)

target_link_libraries(aquila_gate_ws_core
    INTERFACE
        Threads::Threads
        OpenSSL::SSL
        OpenSSL::Crypto
)
