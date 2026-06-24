aux_source_directory(src AERONET_JWT_SRC_LIST)

AeronetAddProjectLibrary(aeronet_jwt
    ${AERONET_JWT_SRC_LIST}
)

# Public includes
target_include_directories(aeronet_jwt
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

find_package(OpenSSL REQUIRED)

# tech (RawChars, base64url, glaze) is public; OpenSSL crypto stays private to the .cpp layer.
target_link_libraries(aeronet_jwt
    PUBLIC
        aeronet_tech
    PRIVATE
        OpenSSL::Crypto
)

# --- Tests ---

AeronetAddUnitTest(
    jwt-algorithm_test
    test/jwt-algorithm_test.cpp
    LIBRARIES
    aeronet_jwt
)

AeronetAddUnitTest(
    jwt-roundtrip_test
    test/jwt-roundtrip_test.cpp
    LIBRARIES
    aeronet_jwt
    OpenSSL::Crypto
)

AeronetAddUnitTest(
    jwt-claims_test
    test/jwt-claims_test.cpp
    LIBRARIES
    aeronet_jwt
)

AeronetAddUnitTest(
    jwt-decode-errors_test
    test/jwt-decode-errors_test.cpp
    LIBRARIES
    aeronet_jwt
    OpenSSL::Crypto
)

AeronetAddUnitTest(
    jwt-jwk_test
    test/jwt-jwk_test.cpp
    LIBRARIES
    aeronet_jwt
    OpenSSL::Crypto
)
