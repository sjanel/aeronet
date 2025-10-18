function(aeronet_http_test NAME)
	set(TGT ${NAME}_test)
	add_project_executable(${TGT} ${TGT}.cpp)
	target_link_libraries(${TGT} PRIVATE aeronet aeronet_test_support GTest::gtest GTest::gmock GTest::gmock_main)
	add_test(NAME ${TGT} COMMAND ${TGT})
endfunction()

# HTTP/1.0 specific behavior tests
aeronet_http_test(http_10)

aeronet_http_test(http_additional)

aeronet_http_test(http_body)

aeronet_http_test(http_partial_head_write)

aeronet_http_test(http_chunked_head)

aeronet_http_test(http_date)

aeronet_http_test(http_errors)

aeronet_http_test(http_head_maxrequests)

aeronet_http_test(http_keepalive)
aeronet_http_test(http_drain)

# Header read timeout test
aeronet_http_test(http_header_timeout)

# Malformed / fuzz-ish header and chunked parsing tests
aeronet_http_test(http_malformed)

aeronet_http_test(http_multi_reuseport)

# Multi server wrapper test
aeronet_http_test(http_multi_wrapper)
aeronet_http_test(multi_http_server)

# OPTIONS, TRACE and CONNECT methods
aeronet_http_test(http_options_trace)
aeronet_http_test(http_connect_success)
aeronet_http_test(http_connect_failures)

# Parser error and fuzz tests
aeronet_http_test(http_parser_errors)

# Routing tests
aeronet_http_test(http_routing)

aeronet_http_test(http_server_move)
aeronet_http_test(http_server_move_invalid)

aeronet_http_test(http_server_restart)

# Trailing slash policy tests
aeronet_http_test(http_trailing_slash)
aeronet_http_test(http_headers_custom)

# Stats exposure test
aeronet_http_test(http_stats)
aeronet_http_test(server_stats_json)

# Inbound request decompression tests (independent of compression feature flags; internal guards handle codecs)
aeronet_http_test(http_request_decompression)

# Compression tests (only meaningful when zlib enabled, but harmless otherwise due to internal guards)
if(AERONET_ENABLE_ZLIB)
  aeronet_http_test(http_compression_zlib_buffered)
  aeronet_http_test(http_compression_zlib_streaming)
endif()

# Zstd compression tests (guarded independently)
if(AERONET_ENABLE_ZSTD)
  aeronet_http_test(http_compression_zstd_buffered)
  aeronet_http_test(http_compression_zstd_streaming)
endif()

# Brotli compression tests (guarded independently)
if(AERONET_ENABLE_BROTLI)
  aeronet_http_test(http_compression_br_buffered)
  aeronet_http_test(http_compression_br_streaming)
endif()

# Streaming tests
aeronet_http_test(http_streaming_backpressure)

aeronet_http_test(http_streaming_keepalive)

aeronet_http_test(http_streaming)
aeronet_http_test(http_streaming_head_content_length)
aeronet_http_test(http_streaming_set_header)
aeronet_http_test(http_streaming_adaptive)

# Mixed streaming + normal handler tests (Phase 2)
aeronet_http_test(http_streaming_mixed)

# Async wrapper test
aeronet_http_test(async_http_server)

aeronet_http_test(http_url_decoding)

# Query parsing tests
aeronet_http_test(http_query_parsing)
aeronet_http_test(http_query_parsing_edge)
aeronet_http_test(http_query_structured_bindings)

aeronet_http_test(test_version)

# Compile-time trait checks
aeronet_http_test(compile_time_traits)

# TLS tests only compiled when OpenSSL support is enabled in the library build.
if (AERONET_ENABLE_OPENSSL)
	aeronet_http_test(http_tls_basic)
	aeronet_http_test(http_tls_negative)
	aeronet_http_test(http_tls_mtls_alpn)
	aeronet_http_test(http_tls_alpn_mismatch)
	aeronet_http_test(http_tls_move_alpn)
	aeronet_http_test(http_tls_cipher_version)
	aeronet_http_test(http_tls_mtls_metrics)
	aeronet_http_test(http_tls_file_certkey)
	aeronet_http_test(http_tls_request_client_cert_optional)
	aeronet_http_test(http_tls_alpn_non_strict)
	aeronet_http_test(http_tls_version_bounds)
	aeronet_http_test(http_tls_cipher_list_invalid)
	aeronet_http_test(http_tls_options_trace)

	# TLS streaming tests (streaming + compression/backpressure over TLS)
	aeronet_http_test(http_tls_streaming)
	aeronet_http_test(http_tls_compression_streaming)
	aeronet_http_test(http_tls_streaming_backpressure)
endif()

# Auto thread-count constructor coverage
# MultiHttpServer convenience constructors (auto, explicit, move, invalid)
aeronet_http_test(multi_http_server_convenience_constructors)
aeronet_http_test(multi_http_server_stress)
aeronet_http_test(multi_http_server_move_running)
aeronet_http_test(multi_http_server_restart)

aeronet_http_test(opentelemetry_integration)

aeronet_http_test(router)

# Maintenance safeguard: warn if any *.cpp test file in this directory is not explicitly added above.
# This helps avoid forgotten tests (e.g., newly created source not registered -> silently untested).
file(GLOB TEST_SOURCES_ABS "${CMAKE_CURRENT_LIST_DIR}/*.cpp")
set(REGISTERED_TEST_SOURCES_REL
	http_10_test.cpp
	http_additional_test.cpp
	http_body_test.cpp
	http_partial_head_write_test.cpp
	http_chunked_head_test.cpp
	http_date_test.cpp
	http_errors_test.cpp
	http_head_maxrequests_test.cpp
	http_keepalive_test.cpp
	http_drain_test.cpp
	http_header_timeout_test.cpp
	http_malformed_test.cpp
	http_multi_reuseport_test.cpp
	http_multi_wrapper_test.cpp
	multi_http_server_test.cpp
	http_options_trace_test.cpp
	http_connect_success_test.cpp
	http_connect_failures_test.cpp
	http_parser_errors_test.cpp
	http_routing_test.cpp
	http_server_move_test.cpp
	http_server_move_invalid_test.cpp
	http_server_restart_test.cpp
	http_stats_test.cpp
	server_stats_json_test.cpp
	http_streaming_backpressure_test.cpp
	http_streaming_keepalive_test.cpp
	http_streaming_test.cpp
	http_streaming_head_content_length_test.cpp
	http_streaming_set_header_test.cpp
	http_streaming_adaptive_test.cpp
	http_streaming_mixed_test.cpp
	http_request_decompression_test.cpp
	async_http_server_test.cpp
	http_url_decoding_test.cpp
	multi_http_server_convenience_constructors_test.cpp
	multi_http_server_stress_test.cpp
	multi_http_server_move_running_test.cpp
	multi_http_server_restart_test.cpp
	multi_http_server_auto_test.cpp
	router_test.cpp
	http_trailing_slash_test.cpp
	http_query_parsing_test.cpp
	http_query_parsing_edge_test.cpp
	http_query_structured_bindings_test.cpp
	http_headers_custom_test.cpp
	test_version_test.cpp
	opentelemetry_integration_test.cpp
	http_compression_zlib_buffered_test.cpp
	http_compression_zlib_streaming_test.cpp
	http_compression_zstd_buffered_test.cpp
	http_compression_zstd_streaming_test.cpp
	http_compression_br_buffered_test.cpp
	http_compression_br_streaming_test.cpp
	compile_time_traits_test.cpp
	
	# TLS tests are intentionally omitted from the always-present list because they are
	# guarded by AERONET_ENABLE_OPENSSL. If OpenSSL is enabled but these files are removed
	# or renamed, add them inside the conditional block above.
)
if (AERONET_ENABLE_OPENSSL)
	list(APPEND REGISTERED_TEST_SOURCES_REL
		http_tls_basic_test.cpp
		http_tls_negative_test.cpp
		http_tls_mtls_alpn_test.cpp
		http_tls_alpn_mismatch_test.cpp
		http_tls_cipher_version_test.cpp
		http_tls_move_alpn_test.cpp
		http_tls_mtls_metrics_test.cpp
		http_tls_file_certkey_test.cpp
		http_tls_request_client_cert_optional_test.cpp
		http_tls_alpn_non_strict_test.cpp
		http_tls_version_bounds_test.cpp
		http_tls_cipher_list_invalid_test.cpp
		http_tls_options_trace_test.cpp
		http_tls_streaming_test.cpp
		http_tls_compression_streaming_test.cpp
		http_tls_streaming_backpressure_test.cpp
	)
endif()

set(UNREGISTERED_FILES "")

foreach(abs ${TEST_SOURCES_ABS})
  get_filename_component(fname ${abs} NAME)
  # All TLS tests follow http_tls_*.cpp naming; when OpenSSL is disabled skip them entirely.
  if (fname MATCHES "^http_tls_.*\\.cpp$")
    if (AERONET_ENABLE_OPENSSL)
      list(FIND REGISTERED_TEST_SOURCES_REL ${fname} idx)
      if (idx EQUAL -1)
        list(APPEND UNREGISTERED_FILES ${fname})
      endif()
    endif()
  else()
    list(FIND REGISTERED_TEST_SOURCES_REL ${fname} idx)
    if(idx EQUAL -1)
      list(APPEND UNREGISTERED_FILES ${fname})
    endif()
  endif()
endforeach()
if(UNREGISTERED_FILES)
	message(FATAL_ERROR "Unregistered test source files detected: ${UNREGISTERED_FILES}. Add aeronet_http_test(...) entries.")
endif()
