aux_source_directory(src/ AERONET_SYS_SRC_LIST)

add_project_library(aeronet_sys
    ${AERONET_SYS_SRC_LIST}
)

target_link_libraries(aeronet_sys
    PUBLIC aeronet_objects aeronet_tech
)

target_include_directories(aeronet_sys
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

add_unit_test(
    base-fd_test
    test/base-fd_test.cpp
    LIBRARIES
    aeronet_sys
)