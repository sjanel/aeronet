# HTTP/2 protocol core (RFC 9113): HPACK, frames, connection and streams, shared by the client and the server. The
# server request handler (Http2ProtocolHandler) is part of the server module.

aux_source_directory(src AERONET_HTTP2_SRC_LIST)

AeronetAddProjectLibrary(aeronet_http2
    ${AERONET_HTTP2_SRC_LIST}
)

target_link_libraries(aeronet_http2
    PUBLIC aeronet_tech aeronet_objects
)

target_include_directories(aeronet_http2
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# Unit tests
AeronetAddUnitTest(
    hpack_test
    test/hpack_test.cpp
    LIBRARIES
    aeronet_http2
)

AeronetAddUnitTest(
    http2-config_test
    test/http2-config_test.cpp
    LIBRARIES
    aeronet_http2
)

AeronetAddUnitTest(
    http2-connection_test
    test/http2-connection_test.cpp
    LIBRARIES
    aeronet_http2
    aeronet_basic_test_support
    aeronet_test_support
)

AeronetAddUnitTest(
    http2-frame_test
    test/http2-frame_test.cpp
    LIBRARIES
    aeronet_http2
)

AeronetAddUnitTest(
    http2-stream_test
    test/http2-stream_test.cpp
    LIBRARIES
    aeronet_http2
)