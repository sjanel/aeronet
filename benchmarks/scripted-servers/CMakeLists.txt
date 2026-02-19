# CMakeLists.txt for scripted-servers benchmarks
#
# Builds standalone HTTP server executables for external benchmarking with wrk.
# These servers are not Google Benchmark executables - they're long-running
# servers meant to be tested with external load generators.

# Aeronet benchmark server (always built when benchmarks are enabled)
AeronetAddProjectExecutable(aeronet-bench-server aeronet_server.cpp)
target_link_libraries(aeronet-bench-server PRIVATE aeronet)
set_target_properties(aeronet-bench-server PROPERTIES FOLDER "benchmarks/servers")

# Enable LTO for release builds
if(AERONET_IPO_SUPPORTED AND CMAKE_BUILD_TYPE STREQUAL "Release")
  set_property(TARGET aeronet-bench-server PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
  message(STATUS "[aeronet][bench] LTO enabled for aeronet-bench-server")
endif()

# Drogon benchmark server (only if drogon is enabled and available)
if(AERONET_BENCH_ENABLE_DROGON OR AERONET_BENCH_ENABLE_PISTACHE OR AERONET_BENCH_ENABLE_CROW)
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

# Copy runner script
configure_file(
  ${SCRIPTED_SERVERS_DIR}/run_benchmarks.py
  ${SCRIPTED_SERVERS_BUILD_DIR}/run_benchmarks.py
  COPYONLY
)

# Copy README
configure_file(
  ${SCRIPTED_SERVERS_DIR}/README.md
  ${SCRIPTED_SERVERS_BUILD_DIR}/README.md
  COPYONLY
)

# Make runner executable
file(CHMOD ${SCRIPTED_SERVERS_BUILD_DIR}/run_benchmarks.py
  PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
              GROUP_READ GROUP_EXECUTE
              WORLD_READ WORLD_EXECUTE
)

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

message(STATUS "[aeronet][bench] Scripted server benchmarks configured")
message(STATUS "[aeronet][bench] After building, run: cd ${SCRIPTED_SERVERS_BUILD_DIR} && ./run_benchmarks.py")
