add_executable(websocket_config_test
    websocket_config_test.cpp
)

target_link_libraries(websocket_config_test
    PRIVATE
        aquila_config
        GTest::gtest_main
)

target_compile_definitions(websocket_config_test
    PRIVATE AQUILA_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

add_test(NAME websocket_config_test
         COMMAND websocket_config_test)

add_test(NAME instrument_catalog_policy_test
         COMMAND bash -c [=[
root="$1"
universe="${root}/config/instruments/usdt_future_universe.csv"
legacy="${root}/config/instruments/usdt_""futures.csv"
legacy_pattern='config/instruments/usdt_''futures\.csv'

if [[ ! -f "${universe}" ]]; then
  printf 'missing universe instrument catalog: %s\n' "${universe}" >&2
  exit 1
fi

if [[ -e "${legacy}" ]]; then
  printf 'legacy instrument catalog must not exist: %s\n' "${legacy}" >&2
  exit 1
fi

if git -C "${root}" grep -n -E "${legacy_pattern}" -- \
    . ':!reports/**' ':!docs/plans/**'; then
  printf 'legacy instrument catalog references remain\n' >&2
  exit 1
fi
]=] _
         ${PROJECT_SOURCE_DIR})

add_executable(data_session_config_test
    data_session_config_test.cpp
)

target_link_libraries(data_session_config_test
    PRIVATE
        aquila_config
        aquila_binance
        aquila_gate
        GTest::gtest_main
        nova
)

target_compile_definitions(data_session_config_test
    PRIVATE AQUILA_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

add_test(NAME data_session_config_test
         COMMAND data_session_config_test)

add_executable(data_reader_config_test
    data_reader_config_test.cpp
)

target_link_libraries(data_reader_config_test
    PRIVATE
        aquila_config
        GTest::gtest_main
        nova
)

target_compile_definitions(data_reader_config_test
    PRIVATE AQUILA_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

add_test(NAME data_reader_config_test
         COMMAND data_reader_config_test)

add_executable(fusion_config_parser_test
    fusion_config_parser_test.cpp
)

target_link_libraries(fusion_config_parser_test
    PRIVATE
        aquila_config
        GTest::gtest_main
        PkgConfig::tomlplusplus
)

add_test(NAME fusion_config_parser_test
         COMMAND fusion_config_parser_test)

add_executable(book_ticker_fusion_config_test
    book_ticker_fusion_config_test.cpp
)

target_link_libraries(book_ticker_fusion_config_test
    PRIVATE
        aquila_config
        GTest::gtest_main
        PkgConfig::tomlplusplus
)

target_compile_definitions(book_ticker_fusion_config_test
    PRIVATE
        AQUILA_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

add_test(NAME book_ticker_fusion_config_test
         COMMAND book_ticker_fusion_config_test)

add_executable(trade_fusion_config_test
    trade_fusion_config_test.cpp
)

target_link_libraries(trade_fusion_config_test
    PRIVATE
        aquila_config
        GTest::gtest_main
        PkgConfig::tomlplusplus
)

target_compile_definitions(trade_fusion_config_test
    PRIVATE
        AQUILA_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

add_test(NAME trade_fusion_config_test
         COMMAND trade_fusion_config_test)

add_executable(order_session_config_test
    order_session_config_test.cpp
)

target_link_libraries(order_session_config_test
    PRIVATE
        aquila_gate
        GTest::gtest_main
        nova
)

target_compile_definitions(order_session_config_test
    PRIVATE AQUILA_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

add_test(NAME order_session_config_test
         COMMAND order_session_config_test)

add_executable(order_gateway_config_test
    order_gateway_config_test.cpp
)

target_link_libraries(order_gateway_config_test
    PRIVATE
        aquila_config
        GTest::gtest_main
)

target_compile_definitions(order_gateway_config_test
    PRIVATE AQUILA_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

add_test(NAME order_gateway_config_test
         COMMAND order_gateway_config_test)

add_executable(order_feedback_shm_config_test
    order_feedback_shm_config_test.cpp
)

target_link_libraries(order_feedback_shm_config_test
    PRIVATE
        aquila_config
        GTest::gtest_main
)

target_compile_definitions(order_feedback_shm_config_test
    PRIVATE AQUILA_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

add_test(NAME order_feedback_shm_config_test
         COMMAND order_feedback_shm_config_test)

add_executable(order_feedback_session_config_test
    order_feedback_session_config_test.cpp
)

target_link_libraries(order_feedback_session_config_test
    PRIVATE
        aquila_gate
        GTest::gtest_main
        nova
)

target_compile_definitions(order_feedback_session_config_test
    PRIVATE AQUILA_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

add_test(NAME order_feedback_session_config_test
         COMMAND order_feedback_session_config_test)

add_executable(strategy_config_test
    strategy_config_test.cpp
)

target_link_libraries(strategy_config_test
    PRIVATE
        aquila_config
        GTest::gtest_main
        nova
)

target_compile_definitions(strategy_config_test
    PRIVATE AQUILA_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

add_test(NAME strategy_config_test
         COMMAND strategy_config_test)
