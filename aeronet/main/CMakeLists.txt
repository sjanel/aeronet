aux_source_directory(src AERONET_SRC_LIST)

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

AeronetAddUnitTest(
    path-handler-entry_test
    test/path-handler-entry_test.cpp
    LIBRARIES
    aeronet
)

AeronetAddUnitTest(
    router_test
    test/router_test.cpp
    LIBRARIES
    aeronet
)

AeronetAddUnitTest(
    websocket-endpoint_test
    test/websocket-endpoint_test.cpp
    LIBRARIES
    aeronet
)