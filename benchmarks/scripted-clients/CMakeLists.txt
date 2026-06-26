# CMakeLists.txt for scripted-clients benchmarks
#
# Builds standalone HTTP *client* drivers benchmarked against a shared, over-provisioned
# aeronet-bench-server (so the server is never the bottleneck and we measure the client cost).
# All drivers share bench-client-harness.hpp and emit a uniform JSON result line; run_client_benchmarks.py
# orchestrates server + drivers across scenarios.

set(SCRIPTED_CLIENTS_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(SCRIPTED_CLIENTS_BUILD_DIR ${CMAKE_CURRENT_BINARY_DIR})

# Helper: apply the common properties (LTO in Release, IDE folder, harness include dir) to a client driver.
function(AeronetConfigureClientDriver target)
  target_include_directories(${target} PRIVATE ${SCRIPTED_CLIENTS_DIR})
  set_target_properties(${target} PROPERTIES FOLDER "benchmarks/clients")
  if(AERONET_IPO_SUPPORTED AND CMAKE_BUILD_TYPE STREQUAL "Release")
    set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    message(STATUS "[aeronet][bench] LTO enabled for ${target}")
  endif()
endfunction()

# The competitor drivers decode the `compress` scenario's gzip body with the SAME codec aeronet uses
# internally — zlib-ng (native mode) — so the content-coding comparison is fair (only the integration
# differs, not the codec). Attach it to a driver that includes bench-client-gzip.hpp.
function(AeronetAttachZlibNg target)
  if(TARGET ZLIBNG::ZLIBNG)
    target_link_libraries(${target} PRIVATE ZLIBNG::ZLIBNG)
    target_compile_definitions(${target} PRIVATE AERONET_BENCH_HAVE_ZLIBNG)
  else()
    message(WARNING "[aeronet][bench] ZLIBNG::ZLIBNG not found; ${target} compress scenario will not decode")
  endif()
endfunction()

set(AERONET_CLIENT_DRIVER_TARGETS)

# --- aeronet HttpClient driver (always built when the client is enabled) ---
AeronetAddProjectExecutable(aeronet-bench-client aeronet_client.cpp)
target_link_libraries(aeronet-bench-client PRIVATE aeronet_client)
AeronetConfigureClientDriver(aeronet-bench-client)
list(APPEND AERONET_CLIENT_DRIVER_TARGETS aeronet-bench-client)
message(STATUS "[aeronet][bench] aeronet client benchmark driver enabled")

# --- libcurl driver (reference C HTTP client) ---
find_package(CURL QUIET)
if(CURL_FOUND)
  AeronetAddProjectExecutable(curl-bench-client curl_client.cpp)
  target_link_libraries(curl-bench-client PRIVATE CURL::libcurl)
  AeronetAttachZlibNg(curl-bench-client)
  AeronetConfigureClientDriver(curl-bench-client)
  list(APPEND AERONET_CLIENT_DRIVER_TARGETS curl-bench-client)
  message(STATUS "[aeronet][bench] libcurl client benchmark driver enabled (libcurl ${CURL_VERSION_STRING})")
else()
  message(STATUS "[aeronet][bench] libcurl client benchmark driver disabled (libcurl not found)")
endif()

# --- Drogon HttpClient driver (sync sendRequest on a per-thread event loop) ---
if(AERONET_BENCH_ENABLE_DROGON AND TARGET drogon)
  AeronetAddProjectExecutable(drogon-bench-client drogon_client.cpp)
  target_link_libraries(drogon-bench-client PRIVATE drogon)
  if(TARGET trantor)
    target_link_libraries(drogon-bench-client PRIVATE trantor)
  endif()
  AeronetAttachZlibNg(drogon-bench-client)
  AeronetConfigureClientDriver(drogon-bench-client)
  list(APPEND AERONET_CLIENT_DRIVER_TARGETS drogon-bench-client)
  message(STATUS "[aeronet][bench] Drogon client benchmark driver enabled")
else()
  message(STATUS "[aeronet][bench] Drogon client benchmark driver disabled (AERONET_BENCH_ENABLE_DROGON=${AERONET_BENCH_ENABLE_DROGON})")
endif()

# --- Boost.Beast driver (synchronous HTTP/1.1 over Asio) ---
# Reuses the Boost include dirs resolved by the scripted-servers beast block (added before this subdir).
if(AERONET_BENCH_ENABLE_BEAST AND AERONET_BOOST_INCLUDE_DIRS)
  find_package(Threads REQUIRED)
  AeronetAddProjectExecutable(beast-bench-client beast_client.cpp)
  target_link_libraries(beast-bench-client PRIVATE Threads::Threads)
  target_include_directories(beast-bench-client SYSTEM PRIVATE ${AERONET_BOOST_INCLUDE_DIRS})
  target_compile_definitions(beast-bench-client PRIVATE BOOST_ASIO_NO_DEPRECATED)
  AeronetAttachZlibNg(beast-bench-client)
  AeronetConfigureClientDriver(beast-bench-client)
  list(APPEND AERONET_CLIENT_DRIVER_TARGETS beast-bench-client)
  message(STATUS "[aeronet][bench] Boost.Beast client benchmark driver enabled")
else()
  message(STATUS "[aeronet][bench] Boost.Beast client benchmark driver disabled (AERONET_BENCH_ENABLE_BEAST=${AERONET_BENCH_ENABLE_BEAST})")
endif()

# Symlink the orchestrator so source edits propagate without reconfiguring.
file(CREATE_LINK
  ${SCRIPTED_CLIENTS_DIR}/run_client_benchmarks.py
  ${SCRIPTED_CLIENTS_BUILD_DIR}/run_client_benchmarks.py
  SYMBOLIC
)

# Copy README for convenience.
configure_file(
  ${SCRIPTED_CLIENTS_DIR}/README.md
  ${SCRIPTED_CLIENTS_BUILD_DIR}/README.md
  COPYONLY
)

# Convenience target: build everything the orchestrator needs, then run it.
add_custom_target(run-scripted-client-benchmarks
  COMMAND ${SCRIPTED_CLIENTS_BUILD_DIR}/run_client_benchmarks.py
  WORKING_DIRECTORY ${SCRIPTED_CLIENTS_BUILD_DIR}
  DEPENDS ${AERONET_CLIENT_DRIVER_TARGETS} aeronet-bench-server
  COMMENT "Running scripted client benchmarks (aeronet HttpClient vs libcurl/drogon/beast)"
)

message(STATUS "[aeronet][bench] Scripted client benchmarks configured")
message(STATUS "[aeronet][bench] After building, run: cd ${SCRIPTED_CLIENTS_BUILD_DIR} && ./run_client_benchmarks.py")
