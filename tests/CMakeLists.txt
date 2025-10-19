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

aeronet_http_test(multi_http_server)

# OPTIONS, TRACE and CONNECT methods
aeronet_http_test(http_options_trace)
aeronet_http_test(http_connect)

# Parser error and fuzz tests
aeronet_http_test(http_parser_errors)

# Routing tests
aeronet_http_test(http_routing)

aeronet_http_test(http_server_lifecycle)

# Trailing slash policy tests
aeronet_http_test(http_trailing_slash)
aeronet_http_test(http_headers)

aeronet_http_test(http_probes)

aeronet_http_test(http_stats)

# Inbound request decompression tests (independent of compression feature flags; internal guards handle codecs)
aeronet_http_test(http_request_decompression)

# Compression tests
if(AERONET_ENABLE_ZLIB OR AERONET_ENABLE_ZSTD OR AERONET_ENABLE_BROTLI)
  aeronet_http_test(http_compression)
endif()

aeronet_http_test(http_streaming)

# Async wrapper test
aeronet_http_test(async_http_server)

aeronet_http_test(http_url_decoding)

aeronet_http_test(http_query_parsing)

# Compile-time trait checks
aeronet_http_test(compile_time_traits)

# TLS tests only compiled when OpenSSL support is enabled in the library build.
if (AERONET_ENABLE_OPENSSL)
	aeronet_http_test(http_tls_handshake)
	aeronet_http_test(http_tls_io)
endif()

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
	multi_http_server_test.cpp
	http_options_trace_test.cpp
	http_connect_test.cpp
	http_parser_errors_test.cpp
	http_routing_test.cpp
	http_server_lifecycle_test.cpp
	http_stats_test.cpp
	http_streaming_test.cpp
	http_request_decompression_test.cpp
	async_http_server_test.cpp
	http_url_decoding_test.cpp
	router_test.cpp
	http_trailing_slash_test.cpp
	http_query_parsing_test.cpp
	http_headers_test.cpp
	http_probes_test.cpp
	opentelemetry_integration_test.cpp
	http_compression_test.cpp
	compile_time_traits_test.cpp
	
	# TLS tests are intentionally omitted from the always-present list because they are
	# guarded by AERONET_ENABLE_OPENSSL. If OpenSSL is enabled but these files are removed
	# or renamed, add them inside the conditional block above.
)
if (AERONET_ENABLE_OPENSSL)
	list(APPEND REGISTERED_TEST_SOURCES_REL
		http_tls_handshake_test.cpp
		http_tls_io_test.cpp
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
