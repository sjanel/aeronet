aux_source_directory(src AERONET_SRC_LIST)

# Exclude the opentelemetry implementation from builds when the feature is disabled.
if(AERONET_ENABLE_OPENTELEMETRY)
    list(REMOVE_ITEM AERONET_SRC_LIST src/void-tracer.cpp)
else()
    list(REMOVE_ITEM AERONET_SRC_LIST src/otel-tracer.cpp)
endif()

add_project_library(aeronet
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

if(AERONET_ENABLE_OPENTELEMETRY)
    # Link opentelemetry targets. We prefer CMake targets over raw .a files
    # because targets establish proper build dependencies. When using FetchContent,
    # targets like 'opentelemetry_trace' ensure the library is built before linking.
    # Make all links PUBLIC so consumers (tests/benchmarks) inherit dependencies.
    
    # Try system-installed opentelemetry-cpp with find_package() double-colon names
    set(_otel_targets_found FALSE)
    if(TARGET opentelemetry::trace)
        target_link_libraries(aeronet PUBLIC opentelemetry::trace)
        set(_otel_targets_found TRUE)
    elseif(TARGET OpenTelemetry::trace)
        target_link_libraries(aeronet PUBLIC OpenTelemetry::trace)
        set(_otel_targets_found TRUE)
    endif()
    
    # If no system package, use FetchContent targets (non-namespaced names).
    # Use PRIVATE linking for FetchContent targets since they're not exported
    # and consumers don't need direct access to opentelemetry internals.
    if(NOT _otel_targets_found AND DEFINED opentelemetry_cpp_SOURCE_DIR)
        # Core SDK trace library
        if(TARGET opentelemetry_trace)
            target_link_libraries(aeronet PRIVATE opentelemetry_trace)
        endif()
        
        # Core SDK metrics library
        if(TARGET opentelemetry_metrics)
            target_link_libraries(aeronet PRIVATE opentelemetry_metrics)
        endif()
        
        # OTLP HTTP exporter and related targets
        if(TARGET opentelemetry_exporter_otlp_http)
            target_link_libraries(aeronet PRIVATE opentelemetry_exporter_otlp_http)
        endif()
        
        # OTLP HTTP metrics exporter
        if(TARGET opentelemetry_exporter_otlp_http_metric)
            target_link_libraries(aeronet PRIVATE opentelemetry_exporter_otlp_http_metric)
        endif()
        
        # Ostream exporter for metrics (used in tests)
        if(TARGET opentelemetry_exporter_ostream_metrics)
            target_link_libraries(aeronet PRIVATE opentelemetry_exporter_ostream_metrics)
        endif()
        if(TARGET opentelemetry_exporter_otlp_http_client)
            target_link_libraries(aeronet PRIVATE opentelemetry_exporter_otlp_http_client)
        endif()
        if(TARGET opentelemetry_otlp_recordable)
            target_link_libraries(aeronet PRIVATE opentelemetry_otlp_recordable)
        endif()
        if(TARGET opentelemetry_proto)
            target_link_libraries(aeronet PRIVATE opentelemetry_proto)
        endif()
        
        # SDK support libraries
        if(TARGET opentelemetry_common)
            target_link_libraries(aeronet PRIVATE opentelemetry_common)
        endif()
        if(TARGET opentelemetry_resources)
            target_link_libraries(aeronet PRIVATE opentelemetry_resources)
        endif()
        if(TARGET opentelemetry_version)
            target_link_libraries(aeronet PRIVATE opentelemetry_version)
        endif()
        
        # HTTP client extension (curl-based)
        if(TARGET http_client_curl)
            target_link_libraries(aeronet PRIVATE http_client_curl)
        endif()
    endif()

    # Link system libraries required by opentelemetry (protobuf and curl).
    # OpenTelemetry's CMake targets should transitively pull these in, but
    # we also link them directly to ensure they're available to consumers.
    find_package(Protobuf REQUIRED)
    if(Protobuf_FOUND)
        target_include_directories(aeronet PUBLIC ${Protobuf_INCLUDE_DIRS})
        target_link_libraries(aeronet PUBLIC ${Protobuf_LIBRARIES})
    endif()
    
    find_package(CURL REQUIRED)
    if(CURL_FOUND)
        target_include_directories(aeronet PUBLIC ${CURL_INCLUDE_DIRS})
        target_link_libraries(aeronet PUBLIC ${CURL_LIBRARIES})
    endif()
endif()
