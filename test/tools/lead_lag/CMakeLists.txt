add_executable(signal_csv_writer_test
    signal_csv_writer_test.cpp
    ${PROJECT_SOURCE_DIR}/tools/lead_lag/signal_csv_writer.cpp
)

target_link_libraries(signal_csv_writer_test
    PRIVATE
        aquila_core
        aquila_strategy
        GTest::gtest_main
        magic_enum::magic_enum
        nova
        quill::quill
)

add_test(NAME signal_csv_writer_test
         COMMAND signal_csv_writer_test)
