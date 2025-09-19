# CMake for aeronet library nested structure
add_library(aeronet
    src/server.cpp
    src/event_loop.cpp
)
add_library(aeronet::aeronet ALIAS aeronet)

target_include_directories(aeronet PUBLIC include)

target_link_libraries(aeronet
    PUBLIC OpenSSL::SSL OpenSSL::Crypto aeronet_tech
)

target_compile_definitions(aeronet
    PUBLIC AERONET_VERSION="${PROJECT_VERSION}"
)
