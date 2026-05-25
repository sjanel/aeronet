aux_source_directory(src AERONET_SRC_LIST)

if(NOT AERONET_ENABLE_TEST_HOOKS)
    list(REMOVE_ITEM AERONET_SRC_LIST src/transport-test-hook.cpp)
endif()

AeronetAddProjectLibrary(aeronet
    ${AERONET_SRC_LIST}
)

add_library(aeronet::aeronet ALIAS aeronet)

target_include_directories(aeronet
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
    target_include_directories(aeronet PUBLIC
        "${opentelemetry_cpp_SOURCE_DIR}/api/include"
        "${opentelemetry_cpp_SOURCE_DIR}/sdk/include"
            "${opentelemetry_cpp_SOURCE_DIR}/exporters/ostream/include"
            "${opentelemetry_cpp_SOURCE_DIR}/exporters/otlp/include"
            "${opentelemetry_cpp_SOURCE_DIR}/ext/include"
    )
endif()

target_link_libraries(aeronet
    PUBLIC aeronet_tech aeronet_sys aeronet_objects aeronet_http
)

if(AERONET_ENABLE_OPENSSL)
    target_link_libraries(aeronet PUBLIC aeronet_tls)
endif()

if(AERONET_ENABLE_HTTP2)
    target_link_libraries(aeronet PUBLIC aeronet_http2)
endif()

AeronetAddUnitTest(
    lifecycle_test
    test/lifecycle_test.cpp
    LIBRARIES
    aeronet_sys
)

AeronetAddUnitTest(
    pending-updates_test
    src/pending-updates.cpp
    test/pending-updates_test.cpp
    LIBRARIES
    aeronet_http
)

AeronetAddUnitTest(
    connection-storage_test
    src/connection-storage.cpp
    test/connection-storage_test.cpp
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
    server-lifecycle-tracker_test
    test/server-lifecycle-tracker_test.cpp
)

AeronetAddUnitTest(
    version_test
    test/version_test.cpp
)

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

AeronetAddUnitTest(
    http-parser_test
    test/http-parser_test.cpp
    LIBRARIES
    aeronet
    aeronet_test_support
)

if(AERONET_ENABLE_TEST_HOOKS)
    target_compile_definitions(aeronet PUBLIC AERONET_ENABLE_TEST_HOOKS)
endif()

if(AERONET_BUILD_TESTS)
    if(AERONET_ENABLE_OPENSSL)
        target_link_libraries(connection-storage_test PUBLIC aeronet_tls)
        target_link_libraries(version_test PUBLIC aeronet_tls)
    else()
        target_link_libraries(version_test PUBLIC aeronet_objects)
    endif()
endif()