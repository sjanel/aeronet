function(aeronet_http_test NAME SOURCE)
	# NAME: logical short name without 'test_' prefix in add_test
	# SOURCE: single cpp file in this directory
	set(TGT ${NAME}_test)
	add_project_executable(${TGT} ${SOURCE})
	target_link_libraries(${TGT} PRIVATE aeronet gtest_main)
	add_test(NAME ${NAME} COMMAND ${TGT})
endfunction()

# HTTP/1.0 specific behavior tests
aeronet_http_test(http_10 http_10.cpp)

aeronet_http_test(http_additional http_additional.cpp)

aeronet_http_test(http_basic http_basic.cpp)

aeronet_http_test(http_chunked_head http_chunked_head.cpp)

aeronet_http_test(http_date http_date.cpp)

aeronet_http_test(http_errors http_errors.cpp)

aeronet_http_test(http_head_maxrequests http_head_maxrequests.cpp)

aeronet_http_test(http_keepalive http_keepalive.cpp)

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

# Stats exposure test
aeronet_http_test(http_stats http_stats.cpp)

# Streaming tests
aeronet_http_test(http_streaming_backpressure http_streaming_backpressure.cpp)

aeronet_http_test(http_streaming_keepalive http_streaming_keepalive.cpp)

aeronet_http_test(http_streaming http_streaming.cpp)
aeronet_http_test(http_streaming_head_content_length http_streaming_head_content_length.cpp)
aeronet_http_test(http_streaming_set_header http_streaming_set_header.cpp)

aeronet_http_test(http_url_decoding_additional http_url_decoding_additional.cpp)

aeronet_http_test(http_url_decoding http_url_decoding.cpp)

# Auto thread-count constructor coverage
# MultiHttpServer convenience constructors (auto, explicit, move, invalid)
aeronet_http_test(multi_http_server_convenience_constructors multi_http_server_convenience_constructors.cpp)

# Maintenance safeguard: warn if any *.cpp test file in this directory is not explicitly added above.
# This helps avoid forgotten tests (e.g., newly created source not registered -> silently untested).
file(GLOB TEST_SOURCES_ABS "${CMAKE_CURRENT_LIST_DIR}/*.cpp")
set(REGISTERED_TEST_SOURCES_REL
	http_10.cpp
	http_additional.cpp
	http_basic.cpp
	http_chunked_head.cpp
	http_date.cpp
	http_errors.cpp
	http_head_maxrequests.cpp
	http_keepalive.cpp
	http_malformed.cpp
	http_multi_reuseport.cpp
	http_multi_wrapper.cpp
	http_parser_errors.cpp
	http_routing.cpp
	http_server_move.cpp
	http_stats.cpp
	http_streaming_backpressure.cpp
	http_streaming_keepalive.cpp
	http_streaming.cpp
		http_streaming_head_content_length.cpp
	http_streaming_set_header.cpp
	http_url_decoding_additional.cpp
	http_url_decoding.cpp
	multi_http_server_convenience_constructors.cpp
	multi_http_server_auto.cpp
)
set(UNREGISTERED_FILES "")
foreach(abs ${TEST_SOURCES_ABS})
	get_filename_component(fname ${abs} NAME)
	list(FIND REGISTERED_TEST_SOURCES_REL ${fname} idx)
	if(idx EQUAL -1)
		list(APPEND UNREGISTERED_FILES ${fname})
	endif()
endforeach()
if(UNREGISTERED_FILES)
	message(FATAL_ERROR "Unregistered test source files detected: ${UNREGISTERED_FILES}. Add aeronet_http_test(...) entries.")
endif()
