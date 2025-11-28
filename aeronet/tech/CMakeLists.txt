# CMake for aeronet library nested structure

aux_source_directory(src TECH_SOURCES)

AeronetAddProjectLibrary(aeronet_tech ${TECH_SOURCES})
target_include_directories(aeronet_tech
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

AeronetAddUnitTest(
    dynamic-concatenated-strings_test
    test/dynamic-concatenated-strings_test.cpp
    LIBRARIES
    aeronet_tech
)

AeronetAddUnitTest(
    flat-hash-map_test
    test/flat-hash-map_test.cpp
    LIBRARIES
    aeronet_tech
)

AeronetAddUnitTest(
    http-constants_test
    test/http-constants_test.cpp
)

AeronetAddUnitTest(
    ipow_test
    test/ipow_test.cpp
)

AeronetAddUnitTest(
    major-minor-version_test
    test/major-minor-version_test.cpp
    LIBRARIES
    aeronet_tech
)

AeronetAddUnitTest(
    ndigits_test
    test/ndigits_test.cpp
)

AeronetAddUnitTest(
    object-pool_test
    test/object-pool_test.cpp
    LIBRARIES
    aeronet_tech
)

AeronetAddUnitTest(
    raw-bytes_test
    test/raw-bytes_test.cpp
    LIBRARIES
    aeronet_tech
)

AeronetAddUnitTest(
    simple-charconv_test
    test/simple-charconv_test.cpp
    LIBRARIES
    aeronet_tech
)

AeronetAddUnitTest(
    static-concatenated-strings_test
    test/static-concatenated-strings_test.cpp
    LIBRARIES
    aeronet_tech
)

AeronetAddUnitTest(
    static-string-view-helpers_test
    test/static-string-view-helpers_test.cpp
)

AeronetAddUnitTest(
    stringconv_test
    test/stringconv_test.cpp
)

AeronetAddUnitTest(
    string-equal-ignore-case_test
    test/string-equal-ignore-case_test.cpp
    LIBRARIES
    aeronet_tech
)

AeronetAddUnitTest(
    timestring_test
    test/timestring_test.cpp
    LIBRARIES
    aeronet_tech
)

AeronetAddUnitTest(
    toupperlower_test
    test/toupperlower_test.cpp
)

AeronetAddUnitTest(
    tchars_test
    test/tchars_test.cpp
)

AeronetAddUnitTest(
    url-encode-decode_test
    test/url-encode-decode_test.cpp
    LIBRARIES
    aeronet_tech
)

AeronetAddUnitTest(
    year-week_test
    test/year-week_test.cpp
    LIBRARIES
    aeronet_tech
)