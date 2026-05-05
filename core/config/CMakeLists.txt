add_library(aquila_config STATIC
    websocket_config.cpp
    websocket_config.h
)

target_include_directories(aquila_config
    PUBLIC ${PROJECT_SOURCE_DIR}
)

target_link_libraries(aquila_config
    PUBLIC
        aquila_core
        PkgConfig::tomlplusplus
    PRIVATE
        nova
)
