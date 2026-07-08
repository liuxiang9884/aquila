add_test(NAME bitget_data_session_help_test
         COMMAND bitget_data_session --help)

add_test(NAME bitget_data_session_dry_run_test
         COMMAND bitget_data_session --config
                 config/data_sessions/bitget_data_session.toml)

set_tests_properties(bitget_data_session_help_test
    bitget_data_session_dry_run_test
    PROPERTIES
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
