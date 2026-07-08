add_library(aquila_bitget INTERFACE)

target_include_directories(aquila_bitget
    INTERFACE
        ${PROJECT_SOURCE_DIR}
)

target_link_libraries(aquila_bitget
    INTERFACE
        aquila_core
)
