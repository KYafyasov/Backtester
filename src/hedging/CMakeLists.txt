file(GLOB SRC CONFIGURE_DEPENDS *.cpp)

set(TARGET back-tester-hedging)
add_library(${TARGET} STATIC ${SRC})

# Hedging is one layer above pricing: it consumes greeks to decide hedges. It
# may also depend on backtester runtime libs (order_book, common) as it grows,
# but pricing must stay a clean leaf, so add those here rather than there.
target_link_libraries(${TARGET} PUBLIC
    back-tester-pricing
    back-tester-types
)

add_library(cmf::hedging ALIAS ${TARGET})
