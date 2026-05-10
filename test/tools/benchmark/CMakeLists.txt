add_executable(time_series_data_test
    time_series_data_test.cpp
)

target_link_libraries(time_series_data_test
    PRIVATE
        aquila_core
        GTest::gtest_main
)

add_test(NAME time_series_data_test
         COMMAND time_series_data_test)
