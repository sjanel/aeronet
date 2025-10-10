aux_source_directory(src AERONET_TEST_SUPPORT_SRC_LIST)

if(NOT AERONET_ENABLE_OPENSSL)
  list(REMOVE_ITEM AERONET_TEST_SUPPORT_SRC_LIST src/test_tls_client.cpp)
  list(REMOVE_ITEM AERONET_TEST_SUPPORT_SRC_LIST src/test_tls_helper.cpp)
endif()

add_project_library(aeronet_test_support
    ${AERONET_TEST_SUPPORT_SRC_LIST}
)

target_include_directories(aeronet_test_support PUBLIC include)

target_link_libraries(aeronet_test_support
    PUBLIC aeronet_tech aeronet_sys aeronet_objects aeronet_http
)

if(AERONET_ENABLE_OPENSSL)
    target_link_libraries(aeronet_test_support PUBLIC aeronet_tls)
endif()
