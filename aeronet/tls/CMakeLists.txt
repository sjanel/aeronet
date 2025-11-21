aux_source_directory(src AERONET_TLS_SRC_LIST)

AeronetAddProjectLibrary(aeronet_tls
    ${AERONET_TLS_SRC_LIST}
)

# Public includes
target_include_directories(aeronet_tls
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../main/include>
        $<INSTALL_INTERFACE:include>
)

find_package(OpenSSL REQUIRED)

# Link against OpenSSL::SSL and OpenSSL::Crypto (imported by find_package) and base tech lib for utils.
target_link_libraries(aeronet_tls
    PRIVATE
        OpenSSL::SSL
        OpenSSL::Crypto
    PUBLIC
        aeronet_sys
        aeronet_objects
        aeronet_tech
)
