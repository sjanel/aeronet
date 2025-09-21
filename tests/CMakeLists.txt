add_project_executable(test_http_basic http_basic.cpp)
target_link_libraries(test_http_basic PRIVATE aeronet gtest_main)
add_test(NAME http_basic COMMAND test_http_basic)

add_project_executable(test_http_server_move http_server_move.cpp)
target_link_libraries(test_http_server_move PRIVATE aeronet gtest_main)
add_test(NAME http_server_move COMMAND test_http_server_move)

add_project_executable(test_http_multi_reuseport http_multi_reuseport.cpp)
target_link_libraries(test_http_multi_reuseport PRIVATE aeronet gtest_main)
add_test(NAME http_multi_reuseport COMMAND test_http_multi_reuseport)

add_project_executable(test_http_keepalive http_keepalive.cpp)
target_link_libraries(test_http_keepalive PRIVATE aeronet gtest_main)
add_test(NAME http_keepalive COMMAND test_http_keepalive)

add_project_executable(test_http_chunked_head http_chunked_head.cpp)
target_link_libraries(test_http_chunked_head PRIVATE aeronet gtest_main)
add_test(NAME http_chunked_head COMMAND test_http_chunked_head)

add_project_executable(test_http_errors http_errors.cpp)
target_link_libraries(test_http_errors PRIVATE aeronet gtest_main)
add_test(NAME http_errors COMMAND test_http_errors)

add_project_executable(test_http_additional http_additional.cpp)
target_link_libraries(test_http_additional PRIVATE aeronet gtest_main)
add_test(NAME http_additional COMMAND test_http_additional)

# HTTP/1.0 specific behavior tests
add_project_executable(test_http_10 http_10.cpp)
target_link_libraries(test_http_10 PRIVATE aeronet gtest_main)
add_test(NAME http_10 COMMAND test_http_10)

# Date header behavior tests
add_project_executable(test_http_date http_date.cpp)
target_link_libraries(test_http_date PRIVATE aeronet gtest_main)
add_test(NAME http_date COMMAND test_http_date)

# HEAD maxRequests test
add_project_executable(test_http_head_maxrequests http_head_maxrequests.cpp)
target_link_libraries(test_http_head_maxrequests PRIVATE aeronet gtest_main)
add_test(NAME http_head_maxrequests COMMAND test_http_head_maxrequests)

# Malformed / fuzz-ish header and chunked parsing tests
add_project_executable(test_http_malformed http_malformed.cpp)
target_link_libraries(test_http_malformed PRIVATE aeronet gtest_main)
add_test(NAME http_malformed COMMAND test_http_malformed)

# Parser error and fuzz tests
add_project_executable(test_http_parser_errors http_parser_errors.cpp)
target_link_libraries(test_http_parser_errors PRIVATE aeronet gtest_main)
add_test(NAME http_parser_errors COMMAND test_http_parser_errors)

# Stats exposure test
add_project_executable(test_http_stats http_stats.cpp)
target_link_libraries(test_http_stats PRIVATE aeronet gtest_main)
add_test(NAME http_stats COMMAND test_http_stats)

# Routing tests
add_project_executable(test_http_routing http_routing.cpp)
target_link_libraries(test_http_routing PRIVATE aeronet gtest_main)
add_test(NAME http_routing COMMAND test_http_routing)

# Multi server wrapper test
add_project_executable(test_http_multi_wrapper http_multi_wrapper.cpp)
target_link_libraries(test_http_multi_wrapper PRIVATE aeronet gtest_main)
add_test(NAME http_multi_wrapper COMMAND test_http_multi_wrapper)
