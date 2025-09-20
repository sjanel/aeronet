function(add_project_executable name)
  add_executable(${name} ${ARGN})
  if(AERONET_ENABLE_CLANG_TIDY AND CLANG_TIDY)
    set_target_properties(${name} PROPERTIES
      CXX_CLANG_TIDY "clang-tidy;--extra-arg-before=--driver-mode=g++"
    )
  endif()
endfunction()

function(add_project_library name)
  if(AERONET_BUILD_SHARED)
    add_library(${name} SHARED ${ARGN})
  else()
    add_library(${name} STATIC ${ARGN})
  endif()
  if(AERONET_ENABLE_CLANG_TIDY AND CLANG_TIDY)
    set_target_properties(${name} PROPERTIES
      CXX_CLANG_TIDY "clang-tidy;--extra-arg-before=--driver-mode=g++"
    )
  endif()
endfunction()