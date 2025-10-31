# CMake for aeronet library nested structure

aux_source_directory(src TECH_SOURCES)

add_project_library(aeronet_tech ${TECH_SOURCES})
target_include_directories(aeronet_tech
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

add_unit_test(
    duration-format_test
    test/duration-format_test.cpp
)

add_unit_test(
    exception_test
    test/exception_test.cpp
)

add_unit_test(
    ipow_test
    test/ipow_test.cpp
)

add_unit_test(
    major-minor-version_test
    test/major-minor-version_test.cpp
    LIBRARIES
    aeronet_tech
)

add_unit_test(
    ndigits_test
    test/ndigits_test.cpp
)

add_unit_test(
    object-pool_test
    test/object-pool_test.cpp
    LIBRARIES
    aeronet_tech
)

add_unit_test(
    raw-bytes_test
    test/raw-bytes_test.cpp
    LIBRARIES
    aeronet_tech
)

add_unit_test(
    static-string-view-helpers_test
    test/static-string-view-helpers_test.cpp
)

add_unit_test(
    stringconv_test
    test/stringconv_test.cpp
)

add_unit_test(
    time-format_test
    test/time-format_test.cpp
    LIBRARIES
    aeronet_tech
)

add_unit_test(
    timestring_test
    test/timestring_test.cpp
    LIBRARIES
    aeronet_tech
)

add_unit_test(
    toupperlower_test
    test/toupperlower_test.cpp
)

add_unit_test(
    tchars_test
    test/tchars_test.cpp
)

add_unit_test(
    unitsparser_test
    test/unitsparser_test.cpp
    LIBRARIES
    aeronet_tech
)

add_unit_test(
    flat-hash-map_test
    test/flat-hash-map_test.cpp
    LIBRARIES
    aeronet_tech
)

add_unit_test(
    simple-charconv_test
    test/simple-charconv_test.cpp
    LIBRARIES
    aeronet_tech
)

add_unit_test(
    url-encode-decode_test
    test/url-encode-decode_test.cpp
    LIBRARIES
    aeronet_tech
)

add_unit_test(
    year-week_test
    test/year-week_test.cpp
    LIBRARIES
    aeronet_tech
)

add_unit_test(
    concatenated-strings_test
    test/concatenated-strings_test.cpp
    LIBRARIES
    aeronet_tech
)
