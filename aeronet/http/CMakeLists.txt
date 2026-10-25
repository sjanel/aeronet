aux_source_directory(src AERONET_HTTP_SRC_LIST)

if(NOT AERONET_ENABLE_HTTP_SERVER)
    list(REMOVE_ITEM AERONET_HTTP_SRC_LIST src/path-handler-entry.cpp)
    list(REMOVE_ITEM AERONET_HTTP_SRC_LIST src/route-constraint.cpp)
    list(REMOVE_ITEM AERONET_HTTP_SRC_LIST src/route-group.cpp)
    list(REMOVE_ITEM AERONET_HTTP_SRC_LIST src/router.cpp)
endif()

AeronetAddProjectLibrary(aeronet_http
    ${AERONET_HTTP_SRC_LIST}
)

target_link_libraries(aeronet_http
    PUBLIC aeronet_tech aeronet_objects aeronet_sys
)

# The routes store their WebSocket endpoint behind a pointer: only the router sources need its definition.
if(AERONET_ENABLE_WEBSOCKET)
    target_link_libraries(aeronet_http PRIVATE aeronet_websocket)
endif()

target_include_directories(aeronet_http
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

AeronetAddUnitTest(
    cors-policy_test
    test/cors-policy_test.cpp
    LIBRARIES
    aeronet_http
    aeronet_basic_test_support
)

AeronetAddUnitTest(
    header-line-parse_test
    test/header-line-parse_test.cpp
    LIBRARIES
    aeronet_http
)

AeronetAddUnitTest(
    http-codec_test
    test/http-codec_test.cpp
    LIBRARIES
    aeronet_http
    aeronet_test_support
)

AeronetAddUnitTest(
    http-partial-head-write_test
    test/http-partial-head-write_test.cpp
    LIBRARIES
    aeronet_http
)

AeronetAddUnitTest(
    http-response_test
    test/http-response_test.cpp 
    LIBRARIES
    aeronet_http
    aeronet_test_support
)

AeronetAddUnitTest(
    http-request-view_test
    test/http-request-view_test.cpp
    LIBRARIES
    aeronet_http
    aeronet_test_support
)

if(AERONET_ENABLE_HTTP_SERVER)
    AeronetAddUnitTest(
        path-handler-entry_test
        test/path-handler-entry_test.cpp
        LIBRARIES
        aeronet_http
    )

    AeronetAddUnitTest(
        route-constraint_test
        test/route-constraint_test.cpp
        LIBRARIES
        aeronet_http
    )

    AeronetAddUnitTest(
        route-group_test
        test/route-group_test.cpp
        LIBRARIES
        aeronet_http
    )

    AeronetAddUnitTest(
        router_test
        test/router_test.cpp
        LIBRARIES
        aeronet_http
    )

    # The route tests register WebSocket endpoints.
    if(AERONET_BUILD_TESTS AND AERONET_ENABLE_WEBSOCKET)
        foreach(_route_test IN ITEMS path-handler-entry_test route-group_test router_test)
            target_link_libraries(${_route_test} PRIVATE aeronet_websocket)
        endforeach()
    endif()
endif()