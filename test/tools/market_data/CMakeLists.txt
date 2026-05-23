add_executable(data_reader_recorder_test
    data_reader_recorder_test.cpp
)

target_link_libraries(data_reader_recorder_test
    PRIVATE
        aquila_config
        aquila_core
        fmt::fmt-header-only
        GTest::gtest_main
        nova
)

add_test(NAME data_reader_recorder_test
         COMMAND data_reader_recorder_test)
