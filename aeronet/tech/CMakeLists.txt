# CMake for aeronet library nested structure

include(FetchContent)

FetchContent_Declare(
    amadeusamc
    GIT_REPOSITORY https://github.com/AmadeusITGroup/amc.git
    GIT_TAG origin/main
)
FetchContent_MakeAvailable(amadeusamc)

aux_source_directory(src TECH_SOURCES)

add_project_library(aeronet_tech ${TECH_SOURCES})
target_include_directories(aeronet_tech
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(aeronet_tech
    PUBLIC OpenSSL::SSL OpenSSL::Crypto amc::amc
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
    ndigits_test
    test/ndigits_test.cpp
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
    unitsparser_test
    test/unitsparser_test.cpp
    LIBRARIES
    aeronet_tech
)

add_unit_test(
    year-week_test
    test/year-week_test.cpp
    LIBRARIES
    aeronet_tech
)