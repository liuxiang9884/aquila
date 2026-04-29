add_library(aquila_gate INTERFACE)

target_include_directories(aquila_gate
    INTERFACE
        ${PROJECT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/exchange/gate/sbe/generated
)

target_link_libraries(aquila_gate
    INTERFACE
        fmt::fmt-header-only
        yyjson::yyjson
        simdjson::simdjson
)
