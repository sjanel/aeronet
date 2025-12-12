aux_source_directory(src/ AERONET_SYS_SRC_LIST)

AeronetAddProjectLibrary(aeronet_sys
    ${AERONET_SYS_SRC_LIST}
)

target_link_libraries(aeronet_sys
    PUBLIC aeronet_tech
)

target_include_directories(aeronet_sys
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

AeronetAddUnitTest(
    base-fd_test
    test/base-fd_test.cpp
    LIBRARIES
    aeronet_sys
    aeronet_basic_test_support
)

AeronetAddUnitTest(
    connection_test
    test/connection_test.cpp
    LIBRARIES
    aeronet_sys
    aeronet_basic_test_support
)

AeronetAddUnitTest(
    errno-throw_test
    test/errno-throw_test.cpp
)


AeronetAddUnitTest(
    event-fd_test
    test/event-fd_test.cpp
    LIBRARIES
    aeronet_sys
    aeronet_basic_test_support
)

AeronetAddUnitTest(
    event-loop_test
    test/event-loop_test.cpp
    LIBRARIES
    aeronet_sys
    aeronet_basic_test_support
)

AeronetAddUnitTest(
    file_test
    test/file_test.cpp
    LIBRARIES
    aeronet_sys
    aeronet_basic_test_support
)

AeronetAddUnitTest(
    socket_test
    test/socket_test.cpp
    LIBRARIES
    aeronet_sys
)

AeronetAddUnitTest(
    tcp-connector_test
    test/tcp-connector_test.cpp
    LIBRARIES
    aeronet_sys
)

AeronetAddUnitTest(
    transport_test
    test/transport_test.cpp
    LIBRARIES
    aeronet_sys
    aeronet_basic_test_support
)