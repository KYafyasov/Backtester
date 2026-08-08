include(CTest)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/test)

set(TARGET back-tester-tests)
add_executable(${TARGET}
    BasicTypesTest.cpp
    CoreMarketTest.cpp
    CoreContractsTest.cpp
    SchedulerTest.cpp
    TypedSimulatedLOBTest.cpp
    TradingTest.cpp
    ResultsTest.cpp
    RuntimeTest.cpp
    NWayMarketMergerTest.cpp
    TestMain.cpp
)

target_link_libraries(${TARGET} PRIVATE
    back-tester-common
    back-tester-lib
)

add_test(NAME native-unit-tests COMMAND ${TARGET})

add_test(
    NAME cli-missing-path
    COMMAND ${CMAKE_COMMAND}
        -DCLI_PATH=$<TARGET_FILE:back-tester>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckCliUsage.cmake
)

add_test(
    NAME cli-tiny-fixture
    COMMAND $<TARGET_FILE:back-tester> ${CMAKE_CURRENT_SOURCE_DIR}/data/tiny_mbo.jsonl
)

add_test(
    NAME cli-decimal-fixture
    COMMAND ${CMAKE_COMMAND}
        -DCLI_PATH=$<TARGET_FILE:back-tester>
        -DDATA_PATH=${CMAKE_CURRENT_SOURCE_DIR}/data/decimal_mbo.jsonl
        -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckCliDecimal.cmake
)

add_test(
    NAME cli-invalid-unsigned-instrument
    COMMAND $<TARGET_FILE:back-tester>
        ${CMAKE_CURRENT_SOURCE_DIR}/data/invalid_unsigned_instrument.jsonl
)
set_tests_properties(cli-invalid-unsigned-instrument PROPERTIES WILL_FAIL TRUE)

add_test(
    NAME cli-invalid-subnanounit-price
    COMMAND $<TARGET_FILE:back-tester>
        ${CMAKE_CURRENT_SOURCE_DIR}/data/invalid_subnanounit_price.jsonl
)
set_tests_properties(cli-invalid-subnanounit-price PROPERTIES WILL_FAIL TRUE)

add_executable(back-tester-scheduler-benchmark
    SchedulerBenchmark.cpp
)

target_link_libraries(back-tester-scheduler-benchmark PRIVATE
    back-tester-common
    back-tester-lib
)

target_compile_definitions(back-tester-scheduler-benchmark PRIVATE
    BACKTESTER_BENCHMARK_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
)

add_executable(back-tester-price-cross-benchmark
    PriceCrossBenchmark.cpp
)

target_link_libraries(back-tester-price-cross-benchmark PRIVATE
    back-tester-common
    back-tester-lib
)

target_compile_definitions(back-tester-price-cross-benchmark PRIVATE
    BACKTESTER_BENCHMARK_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
)
