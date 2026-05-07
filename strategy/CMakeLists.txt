add_library(aquila_strategy INTERFACE)

target_include_directories(aquila_strategy
    INTERFACE
        ${PROJECT_SOURCE_DIR}
)

target_link_libraries(aquila_strategy
    INTERFACE
        aquila_core
)
