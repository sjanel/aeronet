# Ninja + MSVC outside a VS Developer Command Prompt has no implicit include and library directories, which fails with
# C1083 (standard headers not found) and LNK1104 (ws2_32, mswsock...). AeronetInjectMsvcSystemPaths() derives them from
# the compiler path and the registry and injects them. It does nothing in any other setup.

function(_aeronet_add_include_dir dir label)
  if(EXISTS "${dir}")
    message(STATUS "[aeronet] Injecting ${label} (no VS env): ${dir}")
    include_directories(SYSTEM "${dir}")
  endif()
endfunction()

function(_aeronet_add_link_dir dir label)
  if(EXISTS "${dir}")
    message(STATUS "[aeronet] Injecting ${label} (no VS env): ${dir}")
    link_directories("${dir}")
  endif()
endfunction()

# The compiler is <root>/VC/Tools/MSVC/<ver>/bin/Hostx64/x64/cl.exe: the toolchain root is 4 directories above it.
function(_aeronet_inject_msvc_toolchain_dirs compilerPath)
  set(msvcRoot "${compilerPath}")
  foreach(_ RANGE 1 4)
    get_filename_component(msvcRoot "${msvcRoot}" DIRECTORY)
  endforeach()
  _aeronet_add_include_dir("${msvcRoot}/include" "MSVC include dir")
  # msvcprt, vcruntime...
  _aeronet_add_link_dir("${msvcRoot}/lib/x64" "MSVC lib dir")
endfunction()

# Uses the newest SDK version installed under sdkRoot.
function(_aeronet_inject_windows_sdk_dirs sdkRoot)
  cmake_path(SET sdkRoot NORMALIZE "${sdkRoot}")

  file(GLOB includeVersions "${sdkRoot}/Include/*")
  list(SORT includeVersions ORDER DESCENDING)
  if(includeVersions)
    list(GET includeVersions 0 includeVersion)
    foreach(sub ucrt shared um winrt km)
      _aeronet_add_include_dir("${includeVersion}/${sub}" "WinSDK include")
    endforeach()
  endif()

  file(GLOB umLibDirs "${sdkRoot}/Lib/*/um/x64")
  list(SORT umLibDirs ORDER DESCENDING)
  if(umLibDirs)
    list(GET umLibDirs 0 umLibDir)
    _aeronet_add_link_dir("${umLibDir}" "Windows SDK lib dir")
    # Universal CRT (ucrt.lib, ucrtd.lib): Lib/<ver>/ucrt/x64, next to Lib/<ver>/um/x64
    cmake_path(GET umLibDir PARENT_PATH libVersionDir)
    cmake_path(GET libVersionDir PARENT_PATH libVersionDir)
    _aeronet_add_link_dir("${libVersionDir}/ucrt/x64" "WinSDK ucrt lib dir")
  endif()
endfunction()

function(AeronetInjectMsvcSystemPaths)
  if(NOT (WIN32 AND MSVC AND NOT CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES))
    return()
  endif()
  _aeronet_inject_msvc_toolchain_dirs("${CMAKE_CXX_COMPILER}")

  cmake_host_system_information(
    RESULT sdkRoot
    QUERY WINDOWS_REGISTRY "HKLM/SOFTWARE/Microsoft/Windows Kits/Installed Roots"
    VALUE "KitsRoot10"
    VIEW HOST)
  if(sdkRoot)
    _aeronet_inject_windows_sdk_dirs("${sdkRoot}")
  endif()
endfunction()
