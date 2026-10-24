aux_source_directory(src AERONET_SERVER_SRC_LIST)

if(NOT AERONET_ENABLE_ASYNC_HANDLERS)
    list(REMOVE_ITEM AERONET_SERVER_SRC_LIST src/async-handlers.cpp)
endif()

if(NOT AERONET_ENABLE_HTTP2)
    list(REMOVE_ITEM AERONET_SERVER_SRC_LIST src/http2-connections.cpp)
    list(REMOVE_ITEM AERONET_SERVER_SRC_LIST src/http2-protocol-handler.cpp)
endif()

if(NOT AERONET_ENABLE_OPENSSL)
    list(REMOVE_ITEM AERONET_SERVER_SRC_LIST src/tls-acceptor.cpp)
endif()

if(NOT AERONET_ENABLE_WEBSOCKET)
    list(REMOVE_ITEM AERONET_SERVER_SRC_LIST src/websocket-connections.cpp)
endif()

if(NOT AERONET_ENABLE_GLAZE)
    list(REMOVE_ITEM AERONET_SERVER_SRC_LIST src/server-config-loader.cpp)
endif()

if(NOT AERONET_ENABLE_TEST_HOOKS)
    list(REMOVE_ITEM AERONET_SERVER_SRC_LIST src/transport-test-hook.cpp)
endif()

AeronetAddProjectLibrary(aeronet_server
    ${AERONET_SERVER_SRC_LIST}
)

target_include_directories(aeronet_server
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# If we fetched opentelemetry-cpp via FetchContent, its source tree contains
# public headers under api/include and exporter-specific include dirs. Some
# FetchContent builds don't create imported targets with exported include
# directories consumers can pick up; add the likely include paths so this
# project's TUs can find the headers when AERONET_ENABLE_OPENTELEMETRY is ON.
if(DEFINED opentelemetry_cpp_SOURCE_DIR)
    target_include_directories(aeronet_server PUBLIC
        "${opentelemetry_cpp_SOURCE_DIR}/api/include"
        "${opentelemetry_cpp_SOURCE_DIR}/sdk/include"
            "${opentelemetry_cpp_SOURCE_DIR}/exporters/ostream/include"
            "${opentelemetry_cpp_SOURCE_DIR}/exporters/otlp/include"
            "${opentelemetry_cpp_SOURCE_DIR}/ext/include"
    )
endif()

target_link_libraries(aeronet_server
    PUBLIC aeronet_tech aeronet_sys aeronet_objects aeronet_http aeronet_version
)

if(AERONET_ENABLE_OPENSSL)
    target_link_libraries(aeronet_server PUBLIC aeronet_tls)
endif()

if(AERONET_ENABLE_HTTP2)
    target_link_libraries(aeronet_server PUBLIC aeronet_http2)
endif()

if(AERONET_ENABLE_WEBSOCKET)
    target_link_libraries(aeronet_server PUBLIC aeronet_websocket)
endif()

if(AERONET_ENABLE_TEST_HOOKS)
    target_compile_definitions(aeronet_server PUBLIC AERONET_ENABLE_TEST_HOOKS)
endif()

AeronetAddUnitTest(
    access-log-writer_test
    src/access-log-writer.cpp
    test/access-log-writer_test.cpp
    LIBRARIES
    aeronet_http
    aeronet_sys
    aeronet_tech
    aeronet_test_support
)

# connection-state_test uses socketpair(AF_UNIX), fcntl, and sendfile/pread overrides - Linux only.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    AeronetAddUnitTest(
        connection-state_test
        test/connection-state_test.cpp
        LIBRARIES
        aeronet_server
        aeronet_test_support
    )
endif()

AeronetAddUnitTest(
    connection-storage_test
    src/connection-state.cpp
    src/connection-storage.cpp
    test/connection-storage_test.cpp
    LIBRARIES
    aeronet_http
)

AeronetAddUnitTest(
    date-header-cache_test
    test/date-header-cache_test.cpp
    LIBRARIES
    aeronet_server
)

AeronetAddUnitTest(
    http-error-build_test
    src/http-error-build.cpp
    test/http-error-build_test.cpp
    LIBRARIES
    aeronet_objects
    aeronet_basic_test_support
)

if(AERONET_ENABLE_HTTP2)
    AeronetAddUnitTest(
        http2-protocol-handler_test
        test/http2-protocol-handler_test.cpp
        LIBRARIES
        aeronet_server
        aeronet_test_support
    )
endif()

AeronetAddUnitTest(
    http-parser_test
    test/http-parser_test.cpp
    LIBRARIES
    aeronet_server
    aeronet_test_support
)

AeronetAddUnitTest(
    https-redirect_test
    src/https-redirect.cpp
    test/https-redirect_test.cpp
    LIBRARIES
    aeronet_http
)

AeronetAddUnitTest(
    keep-alive-deadline-queue_test
    src/keep-alive-deadline-queue.cpp
    test/keep-alive-deadline-queue_test.cpp
    LIBRARIES
    aeronet_http
)

AeronetAddUnitTest(
    tunnel-resolver_test
    src/tunnel-resolver.cpp
    test/tunnel-resolver_test.cpp
    LIBRARIES
    aeronet_sys
    aeronet_basic_test_support
)

AeronetAddUnitTest(
    lifecycle_test
    test/lifecycle_test.cpp
    LIBRARIES
    aeronet_sys
)

AeronetAddUnitTest(
    multipart-form-data_test
    src/multipart-form-data.cpp
    test/multipart-form-data_test.cpp
    LIBRARIES
    aeronet_objects
)

AeronetAddUnitTest(
    output-queue_test
    test/output-queue_test.cpp
    LIBRARIES
    aeronet_objects
)

AeronetAddUnitTest(
    pending-updates_test
    src/pending-updates.cpp
    test/pending-updates_test.cpp
    LIBRARIES
    aeronet_http
)

AeronetAddUnitTest(
    rate-limit_test
    src/rate-limit.cpp
    test/rate-limit_test.cpp
    LIBRARIES
    aeronet_tech
)

AeronetAddUnitTest(
    rate-limit-middleware_test
    test/rate-limit-middleware_test.cpp
    LIBRARIES
    aeronet_server
    aeronet_test_support
)

if(AERONET_ENABLE_GLAZE)
    AeronetAddUnitTest(
        server-config-loader_test
        test/server-config-loader_test.cpp
        LIBRARIES
        aeronet
        aeronet_basic_test_support
    )
endif()

AeronetAddUnitTest(
    server-lifecycle-tracker_test
    test/server-lifecycle-tracker_test.cpp
)

AeronetAddUnitTest(
    server-stats_test
    src/server-stats.cpp
    test/server-stats_test.cpp
    LIBRARIES
    aeronet_tech
)

if(AERONET_BUILD_TESTS)
    if(AERONET_ENABLE_OPENSSL)
        target_link_libraries(connection-storage_test PUBLIC aeronet_tls)
        target_link_libraries(keep-alive-deadline-queue_test PUBLIC aeronet_tls)
    endif()
endif()

AeronetAddUnitTest(
    static-file-handler_test
    src/static-file-handler.cpp
    test/static-file-handler_test.cpp
    LIBRARIES
    aeronet_http
    aeronet_test_support
)