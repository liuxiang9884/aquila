add_library(aquila_strategy INTERFACE)

target_include_directories(aquila_strategy
    INTERFACE
        ${PROJECT_SOURCE_DIR}
)

target_link_libraries(aquila_strategy
    INTERFACE
        aquila_core
        absl::flat_hash_map
)

add_library(aquila_strategy_gate INTERFACE)

target_link_libraries(aquila_strategy_gate
    INTERFACE
        aquila_strategy
        aquila_gate
)
