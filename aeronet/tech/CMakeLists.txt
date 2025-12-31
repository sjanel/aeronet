# CMake for aeronet library nested structure

aux_source_directory(src TECH_SOURCES)

AeronetAddProjectLibrary(aeronet_tech ${TECH_SOURCES})
target_include_directories(aeronet_tech
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

AeronetAddUnitTest(
    base64_test
    test/base64_test.cpp
    LIBRARIES
    aeronet_tech
)

AeronetAddUnitTest(
    cctype_test
    test/cctype_test.cpp
)

AeronetAddUnitTest(
    char-hexadecimal-converter_test
    test/char-hexadecimal-converter_test.cpp
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
    aeronet_basic_test_support
)

AeronetAddUnitTest(
    http-constants_test
    test/http-constants_test.cpp
)

AeronetAddUnitTest(
    http2-frame-types_test
    test/http2-frame-types_test.cpp
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
    mime-mappings_test
    src/mime-mappings.cpp
    test/mime-mappings_test.cpp
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
    aeronet_basic_test_support
)

AeronetAddUnitTest(
    raw-bytes_test
    test/raw-bytes_test.cpp
    LIBRARIES
    aeronet_tech
    aeronet_basic_test_support
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

AeronetAddUnitTest(
    malloc-fail-after_test
    test/malloc-fail-after_test.cpp
    LIBRARIES
    aeronet_basic_test_support
)