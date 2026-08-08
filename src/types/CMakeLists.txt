set(TARGET back-tester-types)

# Header-only foundational type aliases (Price, Quantity, NanoTime, Side, ...)
# shared across every engine. Deliberately dependency-free so leaf libraries
# such as the pricing engine can consume it without pulling in any backtester
# runtime code.
add_library(${TARGET} INTERFACE)
target_include_directories(${TARGET} INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_library(cmf::types ALIAS ${TARGET})
