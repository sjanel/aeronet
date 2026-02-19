aux_source_directory(src/ AERONET_WEBSOCKET_SRC_LIST)

if(NOT AERONET_ENABLE_ZLIB)
    list(REMOVE_ITEM AERONET_WEBSOCKET_SRC_LIST src/websocket-deflate.cpp)
endif()

AeronetAddProjectLibrary(aeronet_websocket
    ${AERONET_WEBSOCKET_SRC_LIST}
)

target_link_libraries(aeronet_websocket
    PUBLIC aeronet_tech aeronet_objects
)

if(AERONET_ENABLE_ZLIB)
    if (AERONET_ENABLE_ZLIBNG)
        target_link_libraries(aeronet_websocket
            PUBLIC ZLIBNG::ZLIBNG
        )
    else()
        target_link_libraries(aeronet_websocket
            PUBLIC ZLIB::ZLIB
        )
    endif()
endif()

target_include_directories(aeronet_websocket
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

AeronetAddUnitTest(
    sha1_test
    test/sha1_test.cpp
)

if(AERONET_ENABLE_OPENSSL AND AERONET_BUILD_TESTS)
    find_package(OpenSSL REQUIRED)
    target_link_libraries(sha1_test PRIVATE OpenSSL::Crypto)
endif()

if(AERONET_ENABLE_ZLIB)
    AeronetAddUnitTest(
        websocket-compress_test
        test/websocket-compress_test.cpp
        LIBRARIES
        aeronet_websocket
    )
endif()

AeronetAddUnitTest(
    websocket-deflate_test
    test/websocket-deflate_test.cpp
    LIBRARIES
    aeronet_websocket
)

AeronetAddUnitTest(
    websocket-endpoint_test
    test/websocket-endpoint_test.cpp
    LIBRARIES
    aeronet_websocket
    aeronet_http
)

AeronetAddUnitTest(
    websocket-frame_test
    test/websocket-frame_test.cpp
    LIBRARIES
    aeronet_websocket
)

AeronetAddUnitTest(
    websocket-handler_test
    test/websocket-handler_test.cpp
    LIBRARIES
    aeronet_websocket
    aeronet_http
)

AeronetAddUnitTest(
    websocket-upgrade_test
    test/websocket-upgrade_test.cpp
    LIBRARIES
    aeronet_websocket
)