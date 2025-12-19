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
    # Link opentelemetry targets. Use system package if validated, otherwise FetchContent.
    # The _USE_SYSTEM_OPENTELEMETRY variable is set in Dependencies.cmake after validation.
    
    # Core SDK trace library (prefer namespaced opentelemetry-cpp::trace)
    if(TARGET opentelemetry-cpp::trace)
        target_link_libraries(aeronet_objects PRIVATE opentelemetry-cpp::trace)
    elseif(TARGET opentelemetry_trace)
        target_link_libraries(aeronet_objects PRIVATE opentelemetry_trace)
    endif()

    # Core SDK metrics library
    if(TARGET opentelemetry-cpp::metrics)
        target_link_libraries(aeronet_objects PRIVATE opentelemetry-cpp::metrics)
    elseif(TARGET opentelemetry_metrics)
        target_link_libraries(aeronet_objects PRIVATE opentelemetry_metrics)
    endif()

    # OTLP HTTP exporter and related targets
    if(TARGET opentelemetry-cpp::otlp_http_exporter)
        target_link_libraries(aeronet_objects PRIVATE opentelemetry-cpp::otlp_http_exporter)
    elseif(TARGET opentelemetry_exporter_otlp_http)
        target_link_libraries(aeronet_objects PRIVATE opentelemetry_exporter_otlp_http)
    endif()

    # OTLP HTTP metrics exporter
    if(TARGET opentelemetry-cpp::otlp_http_metric_exporter)
        target_link_libraries(aeronet_objects PRIVATE opentelemetry-cpp::otlp_http_metric_exporter)
    elseif(TARGET opentelemetry_exporter_otlp_http_metric)
        target_link_libraries(aeronet_objects PRIVATE opentelemetry_exporter_otlp_http_metric)
    endif()

    # Ostream exporter for metrics (used in tests)
    if(TARGET opentelemetry-cpp::ostream_metrics_exporter)
        target_link_libraries(aeronet_objects PRIVATE opentelemetry-cpp::ostream_metrics_exporter)
    elseif(TARGET opentelemetry_exporter_ostream_metrics)
        target_link_libraries(aeronet_objects PRIVATE opentelemetry_exporter_ostream_metrics)
    endif()

    if(TARGET opentelemetry-cpp::otlp_http_client)
        target_link_libraries(aeronet_objects PRIVATE opentelemetry-cpp::otlp_http_client)
    elseif(TARGET opentelemetry_exporter_otlp_http_client)
        target_link_libraries(aeronet_objects PRIVATE opentelemetry_exporter_otlp_http_client)
    endif()

    if(TARGET opentelemetry-cpp::otlp_recordable)
        target_link_libraries(aeronet_objects PRIVATE opentelemetry-cpp::otlp_recordable)
    elseif(TARGET opentelemetry_otlp_recordable)
        target_link_libraries(aeronet_objects PRIVATE opentelemetry_otlp_recordable)
    endif()

    if(TARGET opentelemetry-cpp::proto)
        target_link_libraries(aeronet_objects PRIVATE opentelemetry-cpp::proto)
        get_target_property(_otel_proto_includes opentelemetry-cpp::proto INTERFACE_INCLUDE_DIRECTORIES)
        if(_otel_proto_includes)
            target_include_directories(aeronet_objects PUBLIC ${_otel_proto_includes})
        endif()
    elseif(TARGET opentelemetry_proto)
        target_link_libraries(aeronet_objects PRIVATE opentelemetry_proto)
        get_target_property(_otel_proto_includes opentelemetry_proto INTERFACE_INCLUDE_DIRECTORIES)
        if(_otel_proto_includes)
            target_include_directories(aeronet_objects PUBLIC ${_otel_proto_includes})
        endif()
    endif()

    if(TARGET opentelemetry-cpp::logs)
        target_link_libraries(aeronet_objects PRIVATE opentelemetry-cpp::logs)
    elseif(TARGET opentelemetry_logs)
        target_link_libraries(aeronet_objects PRIVATE opentelemetry_logs)
    endif()

    if(TARGET opentelemetry-cpp::ostream_span_exporter)
        target_link_libraries(aeronet_objects PRIVATE opentelemetry-cpp::ostream_span_exporter)
        get_target_property(_otel_exp_includes opentelemetry-cpp::ostream_span_exporter INTERFACE_INCLUDE_DIRECTORIES)
        if(_otel_exp_includes)
            target_include_directories(aeronet_objects PUBLIC ${_otel_exp_includes})
        endif()
    elseif(TARGET opentelemetry_exporter_ostream_span)
        target_link_libraries(aeronet_objects PRIVATE opentelemetry_exporter_ostream_span)
        get_target_property(_otel_exp_includes opentelemetry_exporter_ostream_span INTERFACE_INCLUDE_DIRECTORIES)
        if(_otel_exp_includes)
            target_include_directories(aeronet_objects PUBLIC ${_otel_exp_includes})
        endif()
    endif()

    # SDK support libraries
    if(TARGET opentelemetry-cpp::common)
        target_link_libraries(aeronet_objects PRIVATE opentelemetry-cpp::common)
    elseif(TARGET opentelemetry_common)
        target_link_libraries(aeronet_objects PRIVATE opentelemetry_common)
    endif()
    if(TARGET opentelemetry-cpp::sdk)
        target_link_libraries(aeronet_objects PRIVATE opentelemetry-cpp::sdk)
    elseif(TARGET opentelemetry_sdk)
        target_link_libraries(aeronet_objects PRIVATE opentelemetry_sdk)
    endif()
    if(TARGET opentelemetry-cpp::resources)
        target_link_libraries(aeronet_objects PRIVATE opentelemetry-cpp::resources)
    elseif(TARGET opentelemetry_resources)
        target_link_libraries(aeronet_objects PRIVATE opentelemetry_resources)
    endif()
    if(TARGET opentelemetry-cpp::version)
        target_link_libraries(aeronet_objects PRIVATE opentelemetry-cpp::version)
    elseif(TARGET opentelemetry_version)
        target_link_libraries(aeronet_objects PRIVATE opentelemetry_version)
    endif()

    # HTTP client extension (curl-based)
    if(TARGET opentelemetry-cpp::http_client_curl)
        target_link_libraries(aeronet_objects PRIVATE opentelemetry-cpp::http_client_curl)
    elseif(TARGET http_client_curl)
        target_link_libraries(aeronet_objects PRIVATE http_client_curl)
    endif()

    # Link system libraries required by opentelemetry (protobuf and curl).
    # Ensure protobuf and curl are available to consumers. System-installed OpenTelemetry
    # targets may or may not expose these transitively, so always find and link them.
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
        aeronet_basic_test_support
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
    compression-config_test
    test/compression-config_test.cpp
    LIBRARIES
    aeronet_objects
)

AeronetAddUnitTest(
    decompression-config_test
    src/decompression-config.cpp
    test/decompression-config_test.cpp
)

AeronetAddUnitTest(
    dogstatsd_test
    test/dogstatsd_test.cpp
    src/dogstatsd.cpp
    LIBRARIES
    aeronet_sys
    aeronet_basic_test_support
)

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
)

AeronetAddUnitTest(
    http-payload_test
    test/http-payload_test.cpp
    LIBRARIES
    aeronet_tech
)

AeronetAddUnitTest(
    http-response-data_test
    test/http-response-data_test.cpp
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
    server-stats_test
    test/server-stats_test.cpp
    LIBRARIES
    aeronet_objects
)

AeronetAddUnitTest(
    static-file-config_test
    src/static-file-config.cpp
    test/static-file-config_test.cpp
    LIBRARIES
    aeronet_tech
)

AeronetAddUnitTest(
    opentelemetry-integration_test
    test/opentelemetry-integration_test.cpp
    LIBRARIES
    aeronet_objects
    aeronet_test_support
)

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

if(AERONET_ENABLE_ZLIB)
    AeronetAddUnitTest(
        zlib-encoder-decoder_test
        test/zlib-encoder-decoder_test.cpp
        LIBRARIES
        aeronet_objects
    )
    AeronetAddUnitTest(
        zlib-stream-raii_test
        test/zlib-stream-raii_test.cpp
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
        aeronet_basic_test_support
    )
endif()