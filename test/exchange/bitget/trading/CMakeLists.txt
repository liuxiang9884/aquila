add_executable(bitget_order_codecs_test
    order_codecs_test.cpp
)

target_link_libraries(bitget_order_codecs_test
    PRIVATE
        aquila_bitget
        GTest::gtest_main
)

add_test(NAME bitget_order_codecs_test
         COMMAND bitget_order_codecs_test)

add_executable(bitget_order_request_encoder_test
    order_request_encoder_test.cpp
)

target_link_libraries(bitget_order_request_encoder_test
    PRIVATE
        aquila_bitget
        GTest::gtest_main
)

add_test(NAME bitget_order_request_encoder_test
         COMMAND bitget_order_request_encoder_test)

add_executable(bitget_operation_response_parser_test
    operation_response_parser_test.cpp
)

target_link_libraries(bitget_operation_response_parser_test
    PRIVATE
        aquila_bitget
        GTest::gtest_main
)

add_test(NAME bitget_operation_response_parser_test
         COMMAND bitget_operation_response_parser_test)

add_executable(bitget_order_session_config_test
    order_session_config_test.cpp
)

target_compile_definitions(bitget_order_session_config_test
    PRIVATE
        AQUILA_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

target_link_libraries(bitget_order_session_config_test
    PRIVATE
        aquila_bitget
        GTest::gtest_main
)

add_test(NAME bitget_order_session_config_test
         COMMAND bitget_order_session_config_test)

add_executable(bitget_order_session_test
    order_session_test.cpp
)

target_compile_definitions(bitget_order_session_test
    PRIVATE
        AQUILA_BITGET_ORDER_SESSION_ENABLE_TEST_HOOKS
)

target_link_libraries(bitget_order_session_test
    PRIVATE
        aquila_bitget
        GTest::gtest_main
)

add_test(NAME bitget_order_session_test
         COMMAND bitget_order_session_test)

add_executable(bitget_order_session_runtime_adapter_test
    order_session_runtime_adapter_test.cpp
)

target_compile_definitions(bitget_order_session_runtime_adapter_test
    PRIVATE
        AQUILA_BITGET_ORDER_SESSION_RUNTIME_ADAPTER_ENABLE_TEST_HOOKS
)

target_link_libraries(bitget_order_session_runtime_adapter_test
    PRIVATE
        aquila_bitget
        GTest::gtest_main
)

add_test(NAME bitget_order_session_runtime_adapter_test
         COMMAND bitget_order_session_runtime_adapter_test)

add_executable(bitget_order_feedback_parser_test
    order_feedback_parser_test.cpp
)

target_link_libraries(bitget_order_feedback_parser_test
    PRIVATE
        aquila_bitget
        GTest::gtest_main
)

add_test(NAME bitget_order_feedback_parser_test
         COMMAND bitget_order_feedback_parser_test)

add_executable(bitget_order_feedback_session_test
    order_feedback_session_test.cpp
)

target_compile_definitions(bitget_order_feedback_session_test
    PRIVATE
        AQUILA_BITGET_ORDER_FEEDBACK_SESSION_ENABLE_TEST_HOOKS
)

target_link_libraries(bitget_order_feedback_session_test
    PRIVATE
        aquila_bitget
        GTest::gtest_main
)

add_test(NAME bitget_order_feedback_session_test
         COMMAND bitget_order_feedback_session_test)

add_executable(bitget_order_feedback_session_config_test
    order_feedback_session_config_test.cpp
)

target_compile_definitions(bitget_order_feedback_session_config_test
    PRIVATE
        AQUILA_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

target_link_libraries(bitget_order_feedback_session_config_test
    PRIVATE
        aquila_bitget
        GTest::gtest_main
)

add_test(NAME bitget_order_feedback_session_config_test
         COMMAND bitget_order_feedback_session_config_test)

add_executable(bitget_order_feedback_shm_integration_test
    order_feedback_shm_integration_test.cpp
)

target_link_libraries(bitget_order_feedback_shm_integration_test
    PRIVATE
        aquila_bitget
        GTest::gtest_main
)

add_test(NAME bitget_order_feedback_shm_integration_test
         COMMAND bitget_order_feedback_shm_integration_test)

add_executable(bitget_multi_order_session_gateway_test
    multi_order_session_gateway_test.cpp
)

target_link_libraries(bitget_multi_order_session_gateway_test
    PRIVATE
        aquila_bitget
        aquila_core
        GTest::gtest_main
)

add_test(NAME bitget_multi_order_session_gateway_test
         COMMAND bitget_multi_order_session_gateway_test)
