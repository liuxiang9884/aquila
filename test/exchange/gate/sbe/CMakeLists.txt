add_executable(gate_sbe_generated_test
    gate_sbe_generated_test.cpp
)

target_link_libraries(gate_sbe_generated_test
    PRIVATE
        aquila_gate
        GTest::gtest_main
)

add_test(NAME gate_sbe_generated_test
         COMMAND gate_sbe_generated_test)
