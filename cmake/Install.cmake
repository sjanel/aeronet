# Installation and package config (find_package(aeronet)). Included when AERONET_INSTALL is ON, after the targets.

include(CMakePackageConfigHelpers)
set(AERONET_INSTALL_CMAKE_DIR "lib/cmake/aeronet")

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/aeronetConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)

configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/aeronetConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/aeronetConfig.cmake
  @ONLY
)

# Library modules (aeronet/<module>/include, target aeronet_<module>): always built, then the ones of enabled features.
set(_aeronet_modules http tech objects sys version)
foreach(_pair IN ITEMS HTTP2:http2 HTTP_CLIENT:client HTTP_SERVER:server WEBSOCKET:websocket JWT:jwt OPENSSL:tls)
  string(REPLACE ":" ";" _pair "${_pair}")
  list(GET _pair 0 _feature)
  list(GET _pair 1 _module)
  if(AERONET_ENABLE_${_feature})
    list(APPEND _aeronet_modules ${_module})
  endif()
endforeach()

set(_aeronet_export_targets aeronet)
install(DIRECTORY aeronet/main/include/ DESTINATION include)
foreach(_module IN LISTS _aeronet_modules)
  list(APPEND _aeronet_export_targets aeronet_${_module})
  install(DIRECTORY aeronet/${_module}/include/ DESTINATION include)
endforeach()

install(TARGETS ${_aeronet_export_targets}
  EXPORT aeronetTargets
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
  INCLUDES DESTINATION include
)

# The brotli static libraries built with aeronet are exported too, so that the exported aeronet targets have resolvable
# dependencies. Those of a brotli package are found by aeronetConfig.cmake instead.
if(AERONET_ENABLE_BROTLI AND NOT AERONET_BROTLI_PACKAGE)
  foreach(_brotli_target IN ITEMS brotlicommon brotlidec brotlienc)
    if(TARGET ${_brotli_target})
      install(TARGETS ${_brotli_target} EXPORT aeronetTargets ARCHIVE DESTINATION lib LIBRARY DESTINATION lib)
    endif()
  endforeach()
endif()

# aeronetTargets is not exported when a dependency is built with FetchContent and is not exported itself:
# install(EXPORT) would fail. Such builds should use FetchContent for the whole dependency tree.
if(NOT ((AERONET_ENABLE_OPENTELEMETRY AND DEFINED opentelemetry_cpp_SOURCE_DIR)
        OR (AERONET_ENABLE_GLAZE AND DEFINED glaze_SOURCE_DIR)))
  install(EXPORT aeronetTargets
    NAMESPACE aeronet::
    FILE aeronetTargets.cmake
    DESTINATION ${AERONET_INSTALL_CMAKE_DIR}
  )
endif()

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/aeronetConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/aeronetConfigVersion.cmake"
  DESTINATION ${AERONET_INSTALL_CMAKE_DIR}
)
