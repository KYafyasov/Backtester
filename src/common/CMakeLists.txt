set(TARGET back-tester-common)
file(GLOB SRC CONFIGURE_DEPENDS *.cpp)

add_library(${TARGET} STATIC ${SRC})
add_dependencies(${TARGET} GenerateVersion)

if(BUILD_COMPONENT_PIPELINE)
    target_link_libraries(${TARGET} PUBLIC
        back-tester-types
    )
endif()
