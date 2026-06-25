if(NOT DEFINED LEAD_LAG_REPLAY)
  message(FATAL_ERROR "LEAD_LAG_REPLAY is required")
endif()

if(NOT DEFINED LEAD_LAG_CONFIG)
  message(FATAL_ERROR "LEAD_LAG_CONFIG is required")
endif()

execute_process(
  COMMAND "${LEAD_LAG_REPLAY}"
          --config "${LEAD_LAG_CONFIG}"
          --lag-vol-guard-audit-output "/home/liuxiang/tmp/unused_guard_audit.csv"
          --lag-vol-guard-jump-count 0
  RESULT_VARIABLE result
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)

set(combined_output "${stdout}\n${stderr}")

if(NOT result EQUAL 1)
  message(FATAL_ERROR
          "expected invalid lag-vol guard option to exit 1, got ${result}\n"
          "${combined_output}")
endif()

if(NOT combined_output MATCHES "\\[FAIL\\] --lag-vol-guard-jump-count must be positive")
  message(FATAL_ERROR
          "expected CLI failure message in output\nactual:\n${combined_output}")
endif()

if(NOT combined_output MATCHES "lag_vol_guard_jump_count_error")
  message(FATAL_ERROR
          "expected structured Nova log key in output\nactual:\n${combined_output}")
endif()
