cmake_minimum_required(VERSION 3.28)

add_library(aeronet_module)

target_sources(aeronet_module
    PUBLIC
        FILE_SET CXX_MODULES FILES
            aeronet.cppm
)

# Apply the same project properties (compile definitions, warnings, etc.) as all other targets
AeronetSetProjectProperties(aeronet_module)

target_compile_features(aeronet_module PUBLIC cxx_std_23)

# Explicitly set include directories for angle bracket includes
target_include_directories(aeronet_module PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/aeronet/main/include>
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/aeronet/http/include>
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/aeronet/http2/include>
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/aeronet/tech/include>
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/aeronet/sys/include>
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/aeronet/objects/include>
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/aeronet/tls/include>
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/aeronet/websocket/include>
    $<INSTALL_INTERFACE:include>
)

# Link against the main aeronet library to ensure proper build ordering
# (module builds after the library is complete)
target_link_libraries(aeronet_module PUBLIC aeronet::aeronet amc::amc)

add_library(aeronet::module ALIAS aeronet_module)

# Installation
install(TARGETS aeronet_module
    EXPORT ${PROJECT_NAME}Targets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    FILE_SET CXX_MODULES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/module
)
