aux_source_directory(src AERONET_OBJECTS_SRC)

# Exclude the opentelemetry implementation from builds when the feature is disabled.
if(AERONET_ENABLE_OPENTELEMETRY)
    list(REMOVE_ITEM AERONET_OBJECTS_SRC src/void-tracer.cpp)
else()
    list(REMOVE_ITEM AERONET_OBJECTS_SRC src/otel-tracer.cpp)
endif()

if(NOT AERONET_ENABLE_ZLIB)
  list(REMOVE_ITEM AERONET_OBJECTS_SRC src/zlib-decoder.cpp)
  list(REMOVE_ITEM AERONET_OBJECTS_SRC src/zlib-encoder.cpp)
  list(REMOVE_ITEM AERONET_OBJECTS_SRC src/zlib-stream-raii.cpp)
endif()

if(NOT AERONET_ENABLE_ZSTD)
  list(REMOVE_ITEM AERONET_OBJECTS_SRC src/zstd-encoder.cpp)
  list(REMOVE_ITEM AERONET_OBJECTS_SRC src/zstd-decoder.cpp)
endif()

if(NOT AERONET_ENABLE_BROTLI)
  list(REMOVE_ITEM AERONET_OBJECTS_SRC src/brotli-encoder.cpp)
  list(REMOVE_ITEM AERONET_OBJECTS_SRC src/brotli-decoder.cpp)
endif()

AeronetAddProjectLibrary(aeronet_objects
    ${AERONET_OBJECTS_SRC}
)

target_link_libraries(aeronet_objects
    PUBLIC aeronet_tech aeronet_sys
)

if(AERONET_ENABLE_ZLIB)
    if (AERONET_ENABLE_ZLIBNG)
        target_link_libraries(aeronet_objects
            PUBLIC ZLIBNG::ZLIBNG
        )
    else()
        target_link_libraries(aeronet_objects
            PUBLIC ZLIB::ZLIB
        )
    endif()
endif()

if(AERONET_ENABLE_ZSTD)
    # Detect whichever zstd target name is available in this build context.
    set(_AERONET_ZSTD_TARGET "")
    if(TARGET libzstd_static)
        set(_AERONET_ZSTD_TARGET libzstd_static)
    elseif(TARGET zstd::libzstd)
        set(_AERONET_ZSTD_TARGET zstd::libzstd)
    elseif(TARGET zstd::zstd)
        set(_AERONET_ZSTD_TARGET zstd::zstd)
    elseif(TARGET ZSTD::ZSTD)
        set(_AERONET_ZSTD_TARGET ZSTD::ZSTD)
    endif()
    if(_AERONET_ZSTD_TARGET)
        # PUBLIC: the public headers compression-config.hpp and version.hpp include zstd.h. Its include directories
        # are not copied: those of a package are absolute paths, refused in exported targets inside the source tree,
        # and a Conan alias target (zstd::libzstd) has none of its own.
        target_link_libraries(aeronet_objects PUBLIC ${_AERONET_ZSTD_TARGET})
    endif()
endif()

target_include_directories(aeronet_objects
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

if(AERONET_ENABLE_OPENTELEMETRY)
    # The targets of the opentelemetry-cpp package, or of the downloaded opentelemetry-cpp (see Dependencies.cmake).
    target_link_libraries(aeronet_objects PRIVATE ${AERONET_OPENTELEMETRY_TARGETS})

    # Link system libraries required by opentelemetry (protobuf and curl).
    # Ensure protobuf and curl are available to consumers. System-installed OpenTelemetry
    # targets may or may not expose these transitively, so always find and link them.
    # Only their targets: not the old-style Protobuf_LIBRARIES / CURL_LIBRARIES variables (on Windows with shared
    # protobuf, the module-mode variable is the DLL path, which would cause LNK1107), nor their *_INCLUDE_DIRS, absolute
    # paths refused in exported targets inside the source tree (vcpkg cloned in the checkout).
    find_package(Protobuf REQUIRED)
    find_package(CURL REQUIRED)
    target_link_libraries(aeronet_objects PUBLIC protobuf::libprotobuf CURL::libcurl)
endif()

AeronetAddUnitTest(
    accept-encoding-negotiation_test
    test/accept-encoding-negotiation_test.cpp
    LIBRARIES
    aeronet_objects
)

AeronetAddUnitTest(
    connect-target_test
    test/connect-target_test.cpp
    LIBRARIES
    aeronet_objects
)

if(AERONET_ENABLE_BROTLI)
    AeronetAddUnitTest(
        brotli-encoder-decoder_test
        test/brotli-encoder-decoder_test.cpp
        LIBRARIES
        aeronet_test_support
    )
endif()

AeronetAddUnitTest(
    buffer-cache_test
    test/buffer-cache_test.cpp
    LIBRARIES
    aeronet_objects
)

AeronetAddUnitTest(
    builtin-probes-config_test
    test/builtin-probes-config_test.cpp
    src/builtin-probes-config.cpp
    LIBRARIES
    aeronet_tech
)

AeronetAddUnitTest(
    access-log-config_test
    test/access-log-config_test.cpp
    src/access-log-config.cpp
    LIBRARIES
    aeronet_tech
)

AeronetAddUnitTest(
    compression-config_test
    test/compression-config_test.cpp
    LIBRARIES
    aeronet_objects
)

AeronetAddUnitTest(
    counter-accumulator_test
    test/counter-accumulator_test.cpp
    LIBRARIES
    aeronet_tech
    aeronet_basic_test_support
)

AeronetAddUnitTest(
    gauge-deltas_test
    test/gauge-deltas_test.cpp
    LIBRARIES
    aeronet_tech
    aeronet_basic_test_support
)

AeronetAddUnitTest(
    decompression-config_test
    src/decompression-config.cpp
    test/decompression-config_test.cpp
)

# dogstatsd_test uses AF_UNIX sockets and socket override helpers — Linux only.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    AeronetAddUnitTest(
        dogstatsd_test
        test/dogstatsd_test.cpp
        src/dogstatsd.cpp
        LIBRARIES
        aeronet_sys
        aeronet_basic_test_support
    )
endif()

AeronetAddUnitTest(
    encoding_test
    test/encoding_test.cpp
    LIBRARIES
    aeronet_tech
)

AeronetAddUnitTest(
    http-header_test
    src/http-header.cpp
    test/http-header_test.cpp
    LIBRARIES
    aeronet_tech
    aeronet_basic_test_support
)

AeronetAddUnitTest(
    http-header-is-valid_test
    test/http-header-is-valid_test.cpp
)

AeronetAddUnitTest(
    http-headers-view_test
    test/http-headers-view_test.cpp
    LIBRARIES
    aeronet_objects
    aeronet_basic_test_support
)

AeronetAddUnitTest(
    http-method_test
    test/http-method_test.cpp
    LIBRARIES
    aeronet_tech
)

AeronetAddUnitTest(
    embedded-payload_test
    test/embedded-payload_test.cpp
    LIBRARIES
    aeronet_objects
)

AeronetAddUnitTest(
    http-message-data_test
    test/http-message-data_test.cpp
    LIBRARIES
    aeronet_objects
)

AeronetAddUnitTest(
    protocol-handler_test
    test/protocol-handler_test.cpp
    LIBRARIES
    aeronet_objects
)

AeronetAddUnitTest(
    http-payload_test
    test/http-payload_test.cpp
    LIBRARIES
    aeronet_objects
)

AeronetAddUnitTest(
    http-server-config_test
    test/http-server-config_test.cpp
    LIBRARIES
    aeronet_objects
)

AeronetAddUnitTest(
    major-minor-version_test
    test/major-minor-version_test.cpp
    LIBRARIES
    aeronet_tech
)

AeronetAddUnitTest(
    mergeable-headers_test
    test/mergeable-headers_test.cpp
    LIBRARIES
    aeronet_tech
)

AeronetAddUnitTest(
    static-file-config_test
    src/static-file-config.cpp
    test/static-file-config_test.cpp
    LIBRARIES
    aeronet_tech
    aeronet_basic_test_support
)

AeronetAddUnitTest(
    opentelemetry-integration_test
    test/opentelemetry-integration_test.cpp
    LIBRARIES
    aeronet_objects
    aeronet_test_support
)

if(AERONET_BUILD_TESTS AND AERONET_ENABLE_OPENTELEMETRY)
    target_link_libraries(opentelemetry-integration_test PRIVATE ${AERONET_OPENTELEMETRY_METRICS_TARGET})
endif()

AeronetAddUnitTest(
    request-task_test
    test/request-task_test.cpp
)

AeronetAddUnitTest(
    reserved-headers_test
    test/reserved-headers_test.cpp
    LIBRARIES
    aeronet_tech
)

AeronetAddUnitTest(
    string-trim_test
    test/string-trim_test.cpp
    LIBRARIES
    aeronet_tech
)

AeronetAddUnitTest(
    tchars_test
    test/tchars_test.cpp
)

AeronetAddUnitTest(
    telemetry-config_test
    test/telemetry-config_test.cpp
    LIBRARIES
    aeronet_objects
    aeronet_basic_test_support
)

AeronetAddUnitTest(
    tls-config_test
    test/tls-config_test.cpp
    LIBRARIES
    aeronet_objects
)

AeronetAddUnitTest(
    transfer-coding_test
    test/transfer-coding_test.cpp
    LIBRARIES
    aeronet_tech
)

AeronetAddUnitTest(
    uri-host_test
    src/uri-host.cpp
    test/uri-host_test.cpp
    LIBRARIES
    aeronet_tech
)

if(AERONET_ENABLE_ZLIB)
    AeronetAddUnitTest(
        zlib-encoder-decoder_test
        test/zlib-encoder-decoder_test.cpp
        LIBRARIES
        aeronet_test_support
    )
    
    AeronetAddUnitTest(
        zlib-stream-raii_test
        test/zlib-stream-raii_test.cpp
        LIBRARIES
        aeronet_test_support
    )
endif()

if(AERONET_ENABLE_ZSTD)
    AeronetAddUnitTest(
        zstd-encoder-decoder_test
        test/zstd-encoder-decoder_test.cpp
        LIBRARIES
        aeronet_test_support
    )
endif()
