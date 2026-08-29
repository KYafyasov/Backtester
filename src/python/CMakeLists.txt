set(PYBIND11_FINDPYTHON ON)
find_package(pybind11 CONFIG REQUIRED)

pybind11_add_module(_backtester MODULE bindings.cpp)
target_compile_definitions(_backtester PRIVATE BACK_TESTER_VERSION="${PROJECT_VERSION}")
target_link_libraries(_backtester PRIVATE back-tester-lib)

install(TARGETS _backtester DESTINATION back_tester)
