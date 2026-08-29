execute_process(
    COMMAND "${CLI_PATH}" "${DATA_PATH}"
    RESULT_VARIABLE cli_result
    OUTPUT_VARIABLE cli_stdout
    ERROR_VARIABLE cli_stderr
)

if(NOT cli_result EQUAL 0)
    message(FATAL_ERROR "Expected decimal fixture success, got ${cli_result}: ${cli_stderr}")
endif()

if(NOT cli_stdout MATCHES "best_bid=100250000000")
    message(FATAL_ERROR "Expected exact Databento nanounit ticks, got: ${cli_stdout}")
endif()
