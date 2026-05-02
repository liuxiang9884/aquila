add_executable(core_numeric_test
    numeric_test.cpp
)

target_link_libraries(core_numeric_test
    PRIVATE
        aquila_core
        GTest::gtest_main
)

add_test(NAME core_numeric_test
         COMMAND core_numeric_test)
