# CMake for aeronet library nested structure

aux_source_directory(src/ AERONET_SRC_LIST)

add_project_library(aeronet
    ${AERONET_SRC_LIST}
)

add_library(aeronet::aeronet ALIAS aeronet)

target_include_directories(aeronet
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(aeronet
    PUBLIC aeronet_tech aeronet_sys aeronet_http
)

if(AERONET_ENABLE_OPENSSL)
    target_link_libraries(aeronet PUBLIC aeronet_tls)
endif()

