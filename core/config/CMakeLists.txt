add_library(aquila_config STATIC
    data_reader_config.cpp
    data_reader_config.h
    instrument_catalog.cpp
    instrument_catalog.h
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
        absl::flat_hash_map
    PRIVATE
        unofficial::vincentlaucsb-csv-parser::csv
        nova
)
