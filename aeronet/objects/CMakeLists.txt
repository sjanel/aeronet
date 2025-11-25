aux_source_directory(src AERONET_OBJECTS_SRC)

# Exclude the opentelemetry implementation from builds when the feature is disabled.
if(AERONET_ENABLE_OPENTELEMETRY)
    list(REMOVE_ITEM AERONET_OBJECTS_SRC src/void-tracer.cpp)
else()
    list(REMOVE_ITEM AERONET_OBJECTS_SRC src/otel-tracer.cpp)
endif()

if(NOT AERONET_ENABLE_ZLIB)
  list(REMOVE_ITEM AERONET_OBJECTS_SRC src/zlib-encoder.cpp)
  list(REMOVE_ITEM AERONET_OBJECTS_SRC src/zlib-decoder.cpp)
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

target_include_directories(aeronet_objects
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

if(AERONET_ENABLE_OPENTELEMETRY)
    # Link opentelemetry targets. We prefer CMake targets over raw .a files
    # because targets establish proper build dependencies. When using FetchContent,
    # targets like 'opentelemetry_trace' ensure the library is built before linking.
    # Make all links PUBLIC so consumers (tests/benchmarks) inherit dependencies.

    # Try system-installed opentelemetry-cpp with find_package() double-colon names
    set(_otel_targets_found FALSE)
    if(TARGET opentelemetry::trace)
        target_link_libraries(aeronet_objects PUBLIC opentelemetry::trace)
        set(_otel_targets_found TRUE)
    elseif(TARGET OpenTelemetry::trace)
        target_link_libraries(aeronet_objects PUBLIC OpenTelemetry::trace)
        set(_otel_targets_found TRUE)
    endif()

    # If no system package, use FetchContent targets (non-namespaced names).
    # Use PRIVATE linking for FetchContent targets since they're not exported
    # and consumers don't need direct access to opentelemetry internals.
    if(NOT _otel_targets_found AND DEFINED opentelemetry_cpp_SOURCE_DIR)
        # Core SDK trace library
        if(TARGET opentelemetry_trace)
            target_link_libraries(aeronet_objects PRIVATE opentelemetry_trace)
        endif()
        
        # Core SDK metrics library
        if(TARGET opentelemetry_metrics)
            target_link_libraries(aeronet_objects PRIVATE opentelemetry_metrics)
        endif()
        
        # OTLP HTTP exporter and related targets
        if(TARGET opentelemetry_exporter_otlp_http)
            target_link_libraries(aeronet_objects PRIVATE opentelemetry_exporter_otlp_http)
        endif()
        
        # OTLP HTTP metrics exporter
        if(TARGET opentelemetry_exporter_otlp_http_metric)
            target_link_libraries(aeronet_objects PRIVATE opentelemetry_exporter_otlp_http_metric)
        endif()
        
        # Ostream exporter for metrics (used in tests)
        if(TARGET opentelemetry_exporter_ostream_metrics)
            target_link_libraries(aeronet_objects PRIVATE opentelemetry_exporter_ostream_metrics)
        endif()
        if(TARGET opentelemetry_exporter_otlp_http_client)
            target_link_libraries(aeronet_objects PRIVATE opentelemetry_exporter_otlp_http_client)
        endif()
        if(TARGET opentelemetry_otlp_recordable)
            target_link_libraries(aeronet_objects PRIVATE opentelemetry_otlp_recordable)
        endif()
        if(TARGET opentelemetry_proto)
            target_link_libraries(aeronet_objects PRIVATE opentelemetry_proto)
            get_target_property(_otel_proto_includes opentelemetry_proto INTERFACE_INCLUDE_DIRECTORIES)
            if(_otel_proto_includes)
                target_include_directories(aeronet_objects PUBLIC ${_otel_proto_includes})
            endif()
        endif()
        
        # SDK support libraries
        if(TARGET opentelemetry_common)
            target_link_libraries(aeronet_objects PRIVATE opentelemetry_common)
        endif()
        if(TARGET opentelemetry_resources)
            target_link_libraries(aeronet_objects PRIVATE opentelemetry_resources)
        endif()
        if(TARGET opentelemetry_version)
            target_link_libraries(aeronet_objects PRIVATE opentelemetry_version)
        endif()
        
        # HTTP client extension (curl-based)
        if(TARGET http_client_curl)
            target_link_libraries(aeronet_objects PRIVATE http_client_curl)
        endif()
    endif()

    # Link system libraries required by opentelemetry (protobuf and curl).
    # OpenTelemetry's CMake targets should transitively pull these in, but
    # we also link them directly to ensure they're available to consumers.
    find_package(Protobuf REQUIRED)
    if(Protobuf_FOUND)
        target_include_directories(aeronet_objects PUBLIC ${Protobuf_INCLUDE_DIRS})
        target_link_libraries(aeronet_objects PUBLIC ${Protobuf_LIBRARIES})
    endif()

    find_package(CURL REQUIRED)
    if(CURL_FOUND)
        target_include_directories(aeronet_objects PUBLIC ${CURL_INCLUDE_DIRS})
        target_link_libraries(aeronet_objects PUBLIC ${CURL_LIBRARIES})
    endif()
endif()

AeronetAddUnitTest(
    accept-encoding-negotiation_test
    test/accept-encoding-negotiation_test.cpp
    LIBRARIES
    aeronet_objects
)

AeronetAddUnitTest(
    aeronet-version_test
    test/aeronet-version_test.cpp
    LIBRARIES
    aeronet_tech
)

if(AERONET_ENABLE_BROTLI)
    AeronetAddUnitTest(
        brotli-encoder-decoder_test
        test/brotli-encoder-decoder_test.cpp
        LIBRARIES
        aeronet_objects
    )
endif()

AeronetAddUnitTest(
    builtin-probes-config_test
    test/builtin-probes-config_test.cpp
    src/builtin-probes-config.cpp
    LIBRARIES
    aeronet_tech
)

AeronetAddUnitTest(
    http-payload_test
    test/http-payload_test.cpp
    LIBRARIES
    aeronet_tech
)

AeronetAddUnitTest(
    http-server-config_test
    test/http-server-config_test.cpp
    LIBRARIES
    aeronet_objects
)

AeronetAddUnitTest(
    extra-config-validations_test
    test/extra-config-validations_test.cpp
    LIBRARIES
    aeronet_objects
)

AeronetAddUnitTest(
    server-stats_test
    test/server-stats_test.cpp
    LIBRARIES
    aeronet_objects
)

AeronetAddUnitTest(
    opentelemetry-integration_test
    test/opentelemetry-integration_test.cpp
    LIBRARIES
    aeronet_objects
    aeronet_test_support
)

AeronetAddUnitTest(
    tls-config_test
    test/tls-config_test.cpp
    LIBRARIES
    aeronet_objects
)

if(AERONET_ENABLE_ZLIB)
    AeronetAddUnitTest(
        zlib-encoder-decoder_test
        test/zlib-encoder-decoder_test.cpp
        LIBRARIES
        aeronet_objects
    )
endif()

if(AERONET_ENABLE_ZSTD)
    AeronetAddUnitTest(
        zstd-encoder-decoder_test
        test/zstd-encoder-decoder_test.cpp
        LIBRARIES
        aeronet_objects
    )
endif()