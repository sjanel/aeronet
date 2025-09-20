aux_source_directory(src/ AERONET_HTTP_SRC_LIST)

add_project_library(aeronet_http
    ${AERONET_HTTP_SRC_LIST}
)

target_link_libraries(aeronet_http
    PUBLIC aeronet_tech
)

target_include_directories(aeronet_http
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)