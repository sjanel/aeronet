add_library(aeronet INTERFACE)

target_include_directories(aeronet
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(aeronet INTERFACE aeronet_server)

if(AERONET_ENABLE_HTTP_CLIENT)
    target_link_libraries(aeronet INTERFACE aeronet_client)
endif()

add_library(aeronet::aeronet ALIAS aeronet)