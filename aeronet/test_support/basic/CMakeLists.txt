aux_source_directory(src AERONET_BASIC_TEST_SUPPORT_SRC_LIST)

if(NOT AERONET_ENABLE_OPENSSL)
  list(REMOVE_ITEM AERONET_BASIC_TEST_SUPPORT_SRC_LIST src/test-tls-helper.cpp)
endif()

AeronetAddProjectLibrary(aeronet_basic_test_support
    ${AERONET_BASIC_TEST_SUPPORT_SRC_LIST}
)

target_include_directories(aeronet_basic_test_support PUBLIC include)

target_link_libraries(aeronet_basic_test_support
    PUBLIC aeronet_tech aeronet_sys aeronet_objects
)
