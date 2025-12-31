aux_source_directory(src AERONET_FULL_TEST_SUPPORT_SRC_LIST)

if(NOT AERONET_ENABLE_OPENSSL)
  list(REMOVE_ITEM AERONET_FULL_TEST_SUPPORT_SRC_LIST src/test_tls_client.cpp src/test_tls_http2_client.cpp)
endif()

if(NOT AERONET_ENABLE_HTTP2)
    list(REMOVE_ITEM AERONET_FULL_TEST_SUPPORT_SRC_LIST src/test_tls_http2_client.cpp)
endif()

AeronetAddProjectLibrary(aeronet_test_support
    ${AERONET_FULL_TEST_SUPPORT_SRC_LIST}
)

target_include_directories(aeronet_test_support
    PUBLIC
        include
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../main/include>
)

target_link_libraries(aeronet_test_support
    PUBLIC 
    aeronet_tech
    aeronet_sys
    aeronet_basic_test_support
    aeronet_objects
    aeronet_http
)

if(AERONET_ENABLE_HTTP2)
    target_link_libraries(aeronet_test_support PUBLIC aeronet_http2)
endif()

if(AERONET_ENABLE_OPENSSL)
    target_link_libraries(aeronet_test_support PUBLIC aeronet_tls)
endif()
