file(GLOB SRC CONFIGURE_DEPENDS *.cpp)

set(TARGET back-tester-pricing)
add_library(${TARGET} STATIC ${SRC})

# Depends only on the header-only type aliases — never on ingestion, order_book
# or any other backtester runtime code. This is what keeps the engine cleanly
# extractable for the trading engine's feature generator.
target_link_libraries(${TARGET} PUBLIC back-tester-types)

# Namespaced alias: external consumers (the trading engine) link cmf::pricing,
# a stable name that survives any future extraction into its own repo.
add_library(cmf::pricing ALIAS ${TARGET})
