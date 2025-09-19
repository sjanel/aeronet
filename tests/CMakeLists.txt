add_executable(test_http_basic http_basic.cpp)
target_link_libraries(test_http_basic PRIVATE aeronet gtest_main)
add_test(NAME http_basic COMMAND test_http_basic)
