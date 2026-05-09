add_executable(core_base_structures_test
    base_structures_test.cpp
)

target_link_libraries(core_base_structures_test
    PRIVATE
        aquila_core
        GTest::gtest_main
)

add_test(NAME core_base_structures_test
         COMMAND core_base_structures_test)
