execute_process(
    COMMAND "${CLI_PATH}"
    RESULT_VARIABLE cli_result
    OUTPUT_VARIABLE cli_stdout
    ERROR_VARIABLE cli_stderr
)

if(NOT cli_result EQUAL 64)
    message(FATAL_ERROR "Expected CLI exit code 64 without a path, got ${cli_result}")
endif()

set(cli_output "${cli_stdout}${cli_stderr}")
if(NOT cli_output MATCHES "Usage: .* <data-path>")
    message(FATAL_ERROR "Expected CLI usage message, got: ${cli_output}")
endif()
