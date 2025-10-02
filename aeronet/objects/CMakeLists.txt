aux_source_directory(src AERONET_OBJECTS_SRC)

if(NOT AERONET_ENABLE_ZLIB)
  list(REMOVE_ITEM AERONET_OBJECTS_SRC src/zlib-encoder.cpp)
endif()

if(NOT AERONET_ENABLE_ZSTD)
  list(REMOVE_ITEM AERONET_OBJECTS_SRC src/zstd-encoder.cpp)
endif()

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

add_unit_test(
    accept-encoding-negotiation_test
    test/accept-encoding-negotiation_test.cpp
    LIBRARIES
    aeronet_objects
)