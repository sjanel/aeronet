# CMakeLists.txt for scripted-servers benchmarks
#
# Builds standalone HTTP server executables for external benchmarking with wrk.
# These servers are not Google Benchmark executables - they're long-running
# servers meant to be tested with external load generators.

# Aeronet benchmark server (always built when benchmarks are enabled)
AeronetAddProjectExecutable(aeronet-bench-server aeronet_server.cpp)
target_link_libraries(aeronet-bench-server PRIVATE aeronet_server)
set_target_properties(aeronet-bench-server PROPERTIES FOLDER "benchmarks/servers")

# Enable LTO for release builds
if(AERONET_IPO_SUPPORTED AND CMAKE_BUILD_TYPE STREQUAL "Release")
  set_property(TARGET aeronet-bench-server PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
  message(STATUS "[aeronet][bench] LTO enabled for aeronet-bench-server")
endif()

# Drogon benchmark server (only if drogon is enabled and available)
if(AERONET_BENCH_ENABLE_DROGON OR AERONET_BENCH_ENABLE_PISTACHE OR AERONET_BENCH_ENABLE_CROW OR AERONET_BENCH_ENABLE_BEAST)
  # zlib-ng (native mode) is already available from the parent build tree via ZLIBNG::ZLIBNG
  if(NOT TARGET ZLIBNG::ZLIBNG)
    message(FATAL_ERROR "ZLIBNG::ZLIBNG target not found. Enable AERONET_ENABLE_ZLIB for benchmark compression support.")
  endif()
endif()

# Drogon benchmark server (only if drogon is enabled and available)
if(AERONET_BENCH_ENABLE_DROGON AND TARGET drogon)
  AeronetAddProjectExecutable(drogon-bench-server drogon_server.cpp)
  target_link_libraries(drogon-bench-server PRIVATE drogon aeronet_objects)
  set_target_properties(drogon-bench-server PROPERTIES FOLDER "benchmarks/servers")
  
  if(AERONET_IPO_SUPPORTED AND CMAKE_BUILD_TYPE STREQUAL "Release")
    set_property(TARGET drogon-bench-server PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    message(STATUS "[aeronet][bench] LTO enabled for drogon-bench-server")
  endif()
  
  message(STATUS "[aeronet][bench] Drogon benchmark server enabled")
else()
  message(STATUS "[aeronet][bench] Drogon benchmark server disabled (AERONET_BENCH_ENABLE_DROGON=${AERONET_BENCH_ENABLE_DROGON})")
endif()

## Pistache benchmark server (build from source with FetchContent for static linking + LTO)
if(AERONET_BENCH_ENABLE_PISTACHE)
  include(FetchContent)
  
  find_package(Threads REQUIRED)
  
  # Fetch Pistache from source - we build it statically to enable LTO
  FetchContent_Declare(
    pistache
    GIT_REPOSITORY https://github.com/pistacheio/pistache.git
    GIT_TAG        ddffda861aa49012dcda28f1362d0339e718cd52
    GIT_SHALLOW    FALSE
  )
  
  # Configure Pistache build options - disable SSL for simple HTTP benchmarking
  set(PISTACHE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(PISTACHE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(PISTACHE_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(PISTACHE_USE_SSL OFF CACHE BOOL "" FORCE)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  
  FetchContent_MakeAvailable(pistache)
  
  AeronetAddProjectExecutable(pistache-bench-server pistache_server.cpp)
  target_link_libraries(pistache-bench-server PRIVATE pistache Threads::Threads aeronet_objects)
  set_target_properties(pistache-bench-server PROPERTIES FOLDER "benchmarks/servers")
  
  # Enable LTO for Release builds (now safe with static linking)
  if(AERONET_IPO_SUPPORTED AND CMAKE_BUILD_TYPE STREQUAL "Release")
    set_property(TARGET pistache-bench-server PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    message(STATUS "[aeronet][bench] LTO enabled for pistache-bench-server")
  endif()
  
  message(STATUS "[aeronet][bench] Pistache benchmark server enabled (built from source with static linking)")
else()
  message(STATUS "[aeronet][bench] Pistache benchmark server disabled (AERONET_BENCH_ENABLE_PISTACHE=${AERONET_BENCH_ENABLE_PISTACHE})")
endif()

## Crow benchmark server (build from source with FetchContent)
if(AERONET_BENCH_ENABLE_CROW)
  include(FetchContent)
  
  find_package(Threads REQUIRED)
  
  # Crow requires standalone ASIO - fetch it first
  FetchContent_Declare(
    asio
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG        asio-1-32-0
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(asio)
  
  # Set ASIO include dir for Crow to find it
  set(ASIO_INCLUDE_DIR ${asio_SOURCE_DIR}/asio/include CACHE PATH "ASIO include directory" FORCE)
  
  # Fetch Crow from source (CrowCpp - maintained fork)
  FetchContent_Declare(
    Crow
    GIT_REPOSITORY https://github.com/CrowCpp/Crow.git
    GIT_TAG        v1.3.0
    GIT_SHALLOW    TRUE
  )
  
  # Crow is header-only, configure options
  set(CROW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(CROW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(CROW_ENABLE_SSL OFF CACHE BOOL "" FORCE)
  set(CROW_ENABLE_COMPRESSION OFF CACHE BOOL "" FORCE)
  
  FetchContent_MakeAvailable(Crow)
  
  AeronetAddProjectExecutable(crow-bench-server crow_server.cpp)
  target_link_libraries(crow-bench-server PRIVATE Crow::Crow Threads::Threads aeronet_objects)
  set_target_properties(crow-bench-server PROPERTIES FOLDER "benchmarks/servers")
  
  # Enable LTO for Release builds
  if(AERONET_IPO_SUPPORTED AND CMAKE_BUILD_TYPE STREQUAL "Release")
    set_property(TARGET crow-bench-server PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    message(STATUS "[aeronet][bench] LTO enabled for crow-bench-server")
  endif()
  
  message(STATUS "[aeronet][bench] Crow benchmark server enabled (built from source)")
else()
  message(STATUS "[aeronet][bench] Crow benchmark server disabled (AERONET_BENCH_ENABLE_CROW=${AERONET_BENCH_ENABLE_CROW})")
endif()

## Boost.Beast benchmark server
if(AERONET_BENCH_ENABLE_BEAST)
  find_path(AERONET_BOOST_INCLUDE_DIR boost/beast.hpp)
  set(AERONET_BOOST_INCLUDE_DIRS)
  if(NOT AERONET_BOOST_INCLUDE_DIR)
    include(FetchContent)
    FetchContent_Declare(
      boost_beast
      URL https://github.com/boostorg/beast/archive/refs/tags/boost-1.91.0.tar.gz
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
      # We only need the headers: point SOURCE_SUBDIR at a non-existent dir so
      # FetchContent_MakeAvailable populates the archive without add_subdirectory().
      SOURCE_SUBDIR aeronet-headers-only
    )
    FetchContent_MakeAvailable(boost_beast)
    set(AERONET_BOOST_INCLUDE_DIR ${boost_beast_SOURCE_DIR}/include)
    list(APPEND AERONET_BOOST_INCLUDE_DIRS ${AERONET_BOOST_INCLUDE_DIR})
    message(STATUS "[aeronet][bench] Boost.Beast headers fetched from GitHub source")
  else()
    list(APPEND AERONET_BOOST_INCLUDE_DIRS ${AERONET_BOOST_INCLUDE_DIR})
    message(STATUS "[aeronet][bench] Boost headers found at ${AERONET_BOOST_INCLUDE_DIR}")
  endif()

  # Boost.Beast depends on other Boost headers (notably boost/asio.hpp).
  # If the selected include path does not provide them, fetch full Boost headers
  # from GitHub module archives as a fallback.
  unset(AERONET_BOOST_ASIO_INCLUDE_DIR CACHE)
  unset(AERONET_BOOST_ASIO_INCLUDE_DIR)
  find_path(AERONET_BOOST_ASIO_INCLUDE_DIR boost/asio.hpp PATHS ${AERONET_BOOST_INCLUDE_DIR} NO_DEFAULT_PATH)
  if(NOT AERONET_BOOST_ASIO_INCLUDE_DIR)
    include(FetchContent)
    set(AERONET_BOOST_REQUIRED_MODULES
      asio
      assert
      bind
      config
      core
      date_time
      smart_ptr
      static_assert
      system
      throw_exception
      type_traits
      winapi
      predef
      optional
      array
      align
      mp11
      move
      logic
      static_string
      container
      container_hash
      describe
      utility
      io
      endian
      intrusive
    )

    foreach(boost_module ${AERONET_BOOST_REQUIRED_MODULES})
      string(REPLACE "/" "_" boost_module_id ${boost_module})
      set(boost_dep_name boost_${boost_module_id})
      FetchContent_Declare(
        ${boost_dep_name}
        URL https://github.com/boostorg/${boost_module}/archive/refs/tags/boost-1.91.0.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        # Headers only: SOURCE_SUBDIR points at a non-existent dir so
        # FetchContent_MakeAvailable skips add_subdirectory() on the Boost module.
        SOURCE_SUBDIR aeronet-headers-only
      )
      FetchContent_MakeAvailable(${boost_dep_name})
      if(EXISTS ${${boost_dep_name}_SOURCE_DIR}/include)
        list(APPEND AERONET_BOOST_INCLUDE_DIRS ${${boost_dep_name}_SOURCE_DIR}/include)
      endif()
    endforeach()

    find_path(AERONET_BOOST_ASIO_INCLUDE_DIR boost/asio.hpp PATHS ${AERONET_BOOST_INCLUDE_DIRS} NO_DEFAULT_PATH)
    if(NOT AERONET_BOOST_ASIO_INCLUDE_DIR)
      message(FATAL_ERROR "[aeronet][bench] Failed to fetch boost/asio.hpp from Boost module archives")
    endif()

    message(STATUS "[aeronet][bench] Boost module headers fetched from GitHub source (for Asio dependencies)")
  endif()

  if(AERONET_BOOST_INCLUDE_DIRS STREQUAL "")
    list(APPEND AERONET_BOOST_INCLUDE_DIRS ${AERONET_BOOST_INCLUDE_DIR})
  endif()

  # Export the resolved Boost include dirs so the scripted-clients beast driver can reuse them without
  # re-running the (potentially fetched-from-source) resolution above.
  set(AERONET_BOOST_INCLUDE_DIRS "${AERONET_BOOST_INCLUDE_DIRS}" CACHE INTERNAL "Boost include dirs for benchmarks")

  find_package(Threads REQUIRED)

  AeronetAddProjectExecutable(beast-bench-server beast_server.cpp)
  target_link_libraries(beast-bench-server PRIVATE Threads::Threads aeronet_objects)
  target_include_directories(beast-bench-server SYSTEM PRIVATE ${AERONET_BOOST_INCLUDE_DIRS})
  target_compile_definitions(beast-bench-server PRIVATE BOOST_ASIO_NO_DEPRECATED)
  set_target_properties(beast-bench-server PROPERTIES FOLDER "benchmarks/servers")

  if(AERONET_IPO_SUPPORTED AND CMAKE_BUILD_TYPE STREQUAL "Release")
    set_property(TARGET beast-bench-server PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    message(STATUS "[aeronet][bench] LTO enabled for beast-bench-server")
  endif()

  message(STATUS "[aeronet][bench] Boost.Beast benchmark server enabled")
else()
  message(STATUS "[aeronet][bench] Boost.Beast benchmark server disabled (AERONET_BENCH_ENABLE_BEAST=${AERONET_BENCH_ENABLE_BEAST})")
endif()

## uWebSockets benchmark server (WebSocket-only comparison target)
if(AERONET_BENCH_ENABLE_UWEBSOCKETS AND DEFINED uwebsockets_SOURCE_DIR AND DEFINED usockets_SOURCE_DIR)
  # uSockets is pure C — enable C compiler
  enable_language(C)

  # Build uSockets as a static library (it is not a CMake project)
  file(GLOB USOCKETS_SOURCES "${usockets_SOURCE_DIR}/src/*.c")
  list(APPEND USOCKETS_SOURCES "${usockets_SOURCE_DIR}/src/eventing/epoll_kqueue.c")
  add_library(usockets_static STATIC ${USOCKETS_SOURCES})
  target_include_directories(usockets_static PUBLIC "${usockets_SOURCE_DIR}/src")
  # uSockets needs LIBUS_NO_SSL unless we link OpenSSL
  target_compile_definitions(usockets_static PUBLIC LIBUS_NO_SSL)
  set_target_properties(usockets_static PROPERTIES FOLDER "benchmarks/deps")

  AeronetAddProjectExecutable(uwebsockets-bench-server uwebsockets_server.cpp)
  target_include_directories(uwebsockets-bench-server PRIVATE
    "${uwebsockets_SOURCE_DIR}/src"
    "${usockets_SOURCE_DIR}/src"
  )
  target_link_libraries(uwebsockets-bench-server PRIVATE usockets_static z)
  set_target_properties(uwebsockets-bench-server PROPERTIES FOLDER "benchmarks/servers")

  if(AERONET_IPO_SUPPORTED AND CMAKE_BUILD_TYPE STREQUAL "Release")
    set_property(TARGET uwebsockets-bench-server PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    message(STATUS "[aeronet][bench] LTO enabled for uwebsockets-bench-server")
  endif()

  message(STATUS "[aeronet][bench] uWebSockets benchmark server enabled (built from source)")
else()
  message(STATUS "[aeronet][bench] uWebSockets benchmark server disabled (AERONET_BENCH_ENABLE_UWEBSOCKETS=${AERONET_BENCH_ENABLE_UWEBSOCKETS})")
endif()

# Copy Lua scripts and runner to build directory for convenience
set(SCRIPTED_SERVERS_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(SCRIPTED_SERVERS_BUILD_DIR ${CMAKE_CURRENT_BINARY_DIR})

# Copy Lua scripts
file(GLOB LUA_SCRIPTS "${SCRIPTED_SERVERS_DIR}/lua/*.lua")
foreach(script ${LUA_SCRIPTS})
  get_filename_component(script_name ${script} NAME)
  configure_file(${script} ${SCRIPTED_SERVERS_BUILD_DIR}/lua/${script_name} COPYONLY)
endforeach()

# Copy body-codec payloads
set(BODY_CODEC_PAYLOADS "${SCRIPTED_SERVERS_DIR}/lua/body_codec_payloads.txt")
if(EXISTS ${BODY_CODEC_PAYLOADS})
  configure_file(${BODY_CODEC_PAYLOADS} ${SCRIPTED_SERVERS_BUILD_DIR}/lua/body_codec_payloads.txt COPYONLY)
endif()

# Symlink runner script so source edits propagate without reconfiguring
file(CREATE_LINK
  ${SCRIPTED_SERVERS_DIR}/run_benchmarks.py
  ${SCRIPTED_SERVERS_BUILD_DIR}/run_benchmarks.py
  SYMBOLIC
)

# Symlink WebSocket runner script so it is invokable from the build directory
file(CREATE_LINK
  ${SCRIPTED_SERVERS_DIR}/run_ws_benchmarks.py
  ${SCRIPTED_SERVERS_BUILD_DIR}/run_ws_benchmarks.py
  SYMBOLIC
)

# Symlink shared benchmark utilities
file(CREATE_LINK
  ${SCRIPTED_SERVERS_DIR}/bench_utils.py
  ${SCRIPTED_SERVERS_BUILD_DIR}/bench_utils.py
  SYMBOLIC
)

# Copy README
configure_file(
  ${SCRIPTED_SERVERS_DIR}/README.md
  ${SCRIPTED_SERVERS_BUILD_DIR}/README.md
  COPYONLY
)

# Source script must be executable (chmod +x run_benchmarks.py)

# Add convenience target to run all scripted benchmarks
add_custom_target(run-scripted-benchmarks
  COMMAND ${SCRIPTED_SERVERS_BUILD_DIR}/run_benchmarks.py
  WORKING_DIRECTORY ${SCRIPTED_SERVERS_BUILD_DIR}
  DEPENDS aeronet-bench-server
  COMMENT "Running scripted server benchmarks with wrk"
)

if(TARGET drogon-bench-server)
  add_dependencies(run-scripted-benchmarks drogon-bench-server)
endif()

if(TARGET pistache-bench-server)
  add_dependencies(run-scripted-benchmarks pistache-bench-server)
endif()

if(TARGET crow-bench-server)
  add_dependencies(run-scripted-benchmarks crow-bench-server)
endif()

if(TARGET beast-bench-server)
  add_dependencies(run-scripted-benchmarks beast-bench-server)
endif()

if(TARGET uwebsockets-bench-server)
  add_dependencies(run-scripted-benchmarks uwebsockets-bench-server)
endif()

message(STATUS "[aeronet][bench] Scripted server benchmarks configured")
message(STATUS "[aeronet][bench] After building, run: cd ${SCRIPTED_SERVERS_BUILD_DIR} && ./run_benchmarks.py")
message(STATUS "[aeronet][bench] WebSocket benchmarks: cd ${SCRIPTED_SERVERS_BUILD_DIR} && ./run_ws_benchmarks.py")
