aux_source_directory(src/ AERONET_OBJECTS_SRC)

add_project_library(aeronet_objects
    ${AERONET_OBJECTS_SRC}
)

target_link_libraries(aeronet_objects
    PUBLIC aeronet_tech
)

target_include_directories(aeronet_objects
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)