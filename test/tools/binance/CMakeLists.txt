add_executable(binance_data_fusion_config_test
    binance_data_fusion_config_test.cpp
    ${PROJECT_SOURCE_DIR}/tools/binance/binance_data_fusion_config.cpp
)

target_link_libraries(binance_data_fusion_config_test
    PRIVATE
        aquila_core
        GTest::gtest_main
        PkgConfig::tomlplusplus
)

add_test(NAME binance_data_fusion_config_test
         COMMAND binance_data_fusion_config_test)
