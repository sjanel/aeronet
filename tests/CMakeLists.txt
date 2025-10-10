function(aeronet_http_test NAME SOURCE)
	# NAME: logical short name without 'test_' prefix in add_test
	# SOURCE: single cpp file in this directory
	set(TGT ${NAME}_test)
	add_project_executable(${TGT} ${SOURCE})
	target_link_libraries(${TGT} PRIVATE aeronet aeronet_test_support GTest::gtest GTest::gmock GTest::gmock_main)
	add_test(NAME ${NAME} COMMAND ${TGT})
endfunction()

# HTTP/1.0 specific behavior tests
aeronet_http_test(http_10 http_10.cpp)

aeronet_http_test(http_additional http_additional.cpp)

aeronet_http_test(http_body http_body.cpp)

aeronet_http_test(http_chunked_head http_chunked_head.cpp)

aeronet_http_test(http_date http_date.cpp)

aeronet_http_test(http_errors http_errors.cpp)

aeronet_http_test(http_head_maxrequests http_head_maxrequests.cpp)

aeronet_http_test(http_keepalive http_keepalive.cpp)

# Header read timeout test
aeronet_http_test(http_header_timeout http_header_timeout.cpp)

# Malformed / fuzz-ish header and chunked parsing tests
aeronet_http_test(http_malformed http_malformed.cpp)

aeronet_http_test(http_multi_reuseport http_multi_reuseport.cpp)

# Multi server wrapper test
aeronet_http_test(http_multi_wrapper http_multi_wrapper.cpp)

# Parser error and fuzz tests
aeronet_http_test(http_parser_errors http_parser_errors.cpp)

# Routing tests
aeronet_http_test(http_routing http_routing.cpp)

aeronet_http_test(http_server_move http_server_move.cpp)
aeronet_http_test(http_server_move_invalid http_server_move_invalid.cpp)

# Trailing slash policy tests
aeronet_http_test(http_trailing_slash http_trailing_slash.cpp)
aeronet_http_test(http_headers_custom http_headers_custom.cpp)

# Stats exposure test
aeronet_http_test(http_stats http_stats.cpp)
aeronet_http_test(server_stats_json server_stats_json.cpp)

# Inbound request decompression tests (independent of compression feature flags; internal guards handle codecs)
aeronet_http_test(http_request_decompression http_request_decompression.cpp)

# Compression tests (only meaningful when zlib enabled, but harmless otherwise due to internal guards)
if(AERONET_ENABLE_ZLIB)
	aeronet_http_test(http_compression_zlib_buffered http_compression_zlib_buffered.cpp)
	aeronet_http_test(http_compression_zlib_streaming http_compression_zlib_streaming.cpp)
endif()

# Zstd compression tests (guarded independently)
if(AERONET_ENABLE_ZSTD)
	aeronet_http_test(http_compression_zstd_buffered http_compression_zstd_buffered.cpp)
	aeronet_http_test(http_compression_zstd_streaming http_compression_zstd_streaming.cpp)
endif()

# Brotli compression tests (guarded independently)
if(AERONET_ENABLE_BROTLI)
	aeronet_http_test(http_compression_br_buffered http_compression_br_buffered.cpp)
	aeronet_http_test(http_compression_br_streaming http_compression_br_streaming.cpp)
endif()

# Streaming tests
aeronet_http_test(http_streaming_backpressure http_streaming_backpressure.cpp)

aeronet_http_test(http_streaming_keepalive http_streaming_keepalive.cpp)

aeronet_http_test(http_streaming http_streaming.cpp)
aeronet_http_test(http_streaming_head_content_length http_streaming_head_content_length.cpp)
aeronet_http_test(http_streaming_set_header http_streaming_set_header.cpp)
aeronet_http_test(http_streaming_adaptive http_streaming_adaptive.cpp)

# Mixed streaming + normal handler tests (Phase 2)
aeronet_http_test(http_streaming_mixed http_streaming_mixed.cpp)

# Async wrapper test
aeronet_http_test(async_http_server async_http_server.cpp)

aeronet_http_test(http_url_decoding_additional http_url_decoding_additional.cpp)

aeronet_http_test(http_url_decoding http_url_decoding.cpp)

# Query parsing tests
aeronet_http_test(http_query_parsing http_query_parsing.cpp)
aeronet_http_test(http_query_parsing_edge http_query_parsing_edge.cpp)
aeronet_http_test(http_query_structured_bindings http_query_structured_bindings.cpp)

aeronet_http_test(test_version test_version.cpp)

# TLS tests only compiled when OpenSSL support is enabled in the library build.
if (AERONET_ENABLE_OPENSSL)
	aeronet_http_test(http_tls_basic http_tls_basic.cpp)
	aeronet_http_test(http_tls_negative http_tls_negative.cpp)
	aeronet_http_test(http_tls_mtls_alpn http_tls_mtls_alpn.cpp)
	aeronet_http_test(http_tls_alpn_mismatch http_tls_alpn_mismatch.cpp)
	aeronet_http_test(http_tls_move_alpn http_tls_move_alpn.cpp)
	aeronet_http_test(http_tls_cipher_version http_tls_cipher_version.cpp)
	aeronet_http_test(http_tls_mtls_metrics http_tls_mtls_metrics.cpp)
	aeronet_http_test(http_tls_file_certkey http_tls_file_certkey.cpp)
	aeronet_http_test(http_tls_request_client_cert_optional http_tls_request_client_cert_optional.cpp)
	aeronet_http_test(http_tls_alpn_non_strict http_tls_alpn_non_strict.cpp)
	aeronet_http_test(http_tls_version_bounds http_tls_version_bounds.cpp)
	aeronet_http_test(http_tls_cipher_list_invalid http_tls_cipher_list_invalid.cpp)
endif()

# Auto thread-count constructor coverage
# MultiHttpServer convenience constructors (auto, explicit, move, invalid)
aeronet_http_test(multi_http_server_convenience_constructors multi_http_server_convenience_constructors.cpp)
aeronet_http_test(multi_http_server_stress multi_http_server_stress.cpp)
aeronet_http_test(multi_http_server_move_running multi_http_server_move_running.cpp)
aeronet_http_test(multi_http_server_restart multi_http_server_restart.cpp)

# Maintenance safeguard: warn if any *.cpp test file in this directory is not explicitly added above.
# This helps avoid forgotten tests (e.g., newly created source not registered -> silently untested).
file(GLOB TEST_SOURCES_ABS "${CMAKE_CURRENT_LIST_DIR}/*.cpp")
set(REGISTERED_TEST_SOURCES_REL
	http_10.cpp
	http_additional.cpp
	http_body.cpp
	http_chunked_head.cpp
	http_date.cpp
	http_errors.cpp
	http_head_maxrequests.cpp
	http_keepalive.cpp
	http_header_timeout.cpp
	http_malformed.cpp
	http_multi_reuseport.cpp
	http_multi_wrapper.cpp
	http_parser_errors.cpp
	http_routing.cpp
	http_server_move.cpp
	http_server_move_invalid.cpp
	http_stats.cpp
	server_stats_json.cpp
	http_streaming_backpressure.cpp
	http_streaming_keepalive.cpp
	http_streaming.cpp
	http_streaming_head_content_length.cpp
	http_streaming_set_header.cpp
	http_streaming_adaptive.cpp
	http_streaming_mixed.cpp
	http_request_decompression.cpp
	async_http_server.cpp
	http_url_decoding_additional.cpp
	http_url_decoding.cpp
	multi_http_server_convenience_constructors.cpp
	multi_http_server_stress.cpp
	multi_http_server_move_running.cpp
	multi_http_server_restart.cpp
	multi_http_server_auto.cpp
	http_trailing_slash.cpp
	http_query_parsing.cpp
	http_query_parsing_edge.cpp
	http_query_structured_bindings.cpp
	http_headers_custom.cpp
	test_version.cpp
	http_compression_zlib_buffered.cpp
	http_compression_zlib_streaming.cpp
	http_compression_zstd_buffered.cpp
	http_compression_zstd_streaming.cpp
	http_compression_br_buffered.cpp
	http_compression_br_streaming.cpp
	# TLS tests are intentionally omitted from the always-present list because they are
	# guarded by AERONET_ENABLE_OPENSSL. If OpenSSL is enabled but these files are removed
	# or renamed, add them inside the conditional block above.
)
if (AERONET_ENABLE_OPENSSL)
  list(APPEND REGISTERED_TEST_SOURCES_REL
    http_tls_basic.cpp
		http_tls_negative.cpp
		http_tls_mtls_alpn.cpp
		http_tls_alpn_mismatch.cpp)
	  list(APPEND REGISTERED_TEST_SOURCES_REL http_tls_cipher_version.cpp)
		list(APPEND REGISTERED_TEST_SOURCES_REL http_tls_move_alpn.cpp)
		list(APPEND REGISTERED_TEST_SOURCES_REL http_tls_mtls_metrics.cpp)
		list(APPEND REGISTERED_TEST_SOURCES_REL http_tls_file_certkey.cpp)
    list(APPEND REGISTERED_TEST_SOURCES_REL http_tls_request_client_cert_optional.cpp)
    list(APPEND REGISTERED_TEST_SOURCES_REL http_tls_alpn_non_strict.cpp)
    list(APPEND REGISTERED_TEST_SOURCES_REL http_tls_version_bounds.cpp)
    list(APPEND REGISTERED_TEST_SOURCES_REL http_tls_cipher_list_invalid.cpp)
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
