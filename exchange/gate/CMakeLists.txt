add_library(aquila_gate INTERFACE)

target_include_directories(aquila_gate
    INTERFACE ${PROJECT_SOURCE_DIR}
)

target_link_libraries(aquila_gate
    INTERFACE
        yyjson::yyjson
)
