set(TARGET back-tester-lib)

add_library(${TARGET} STATIC
    DataIngestionLayer.cpp
    MarketDataEvent.cpp
    LimitOrderBook.cpp
    LobRouter.cpp
)

target_link_libraries(${TARGET} PUBLIC
    back-tester-common
)

add_executable(back-tester
    main.cpp
)

target_link_libraries(back-tester PUBLIC
    back-tester-lib
)

if(BUILD_COMPONENT_PIPELINE)
    # The Feather ingestion prototype from cmf-team/main remains available as
    # a separate executable so the deterministic HW4 JSONL CLI keeps its
    # contract.
    add_executable(back-tester-feather
        feather_main.cpp
        args.cpp
    )

    target_link_libraries(back-tester-feather PUBLIC
        back-tester-ingestion
        back-tester-order-book
        Threads::Threads
    )
endif()
