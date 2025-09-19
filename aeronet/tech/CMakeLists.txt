# CMake for aeronet library nested structure

include(FetchContent)

FetchContent_Declare(
    amadeusamc
    GIT_REPOSITORY https://github.com/AmadeusITGroup/amc.git
    GIT_TAG origin/main
)
FetchContent_MakeAvailable(amadeusamc)

aux_source_directory(src TECH_SOURCES)

add_library(aeronet_tech ${TECH_SOURCES})
target_include_directories(aeronet_tech PUBLIC include)

target_link_libraries(aeronet_tech
    PUBLIC OpenSSL::SSL OpenSSL::Crypto amc::amc
)