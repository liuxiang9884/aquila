add_library(aquila_evaluation INTERFACE)

target_include_directories(aquila_evaluation
    INTERFACE
        ${PROJECT_SOURCE_DIR}
)

target_link_libraries(aquila_evaluation
    INTERFACE
        aquila_core
        aquila_gate
)
