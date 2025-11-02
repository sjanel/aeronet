## cmake/JobPool.cmake
# Compute logical CPU count and system RAM and set JOB_POOLS accordingly.
# Sets two pools:
#  - link_pool: depth = max(1, floor(total_ram_GB / 4))
#  - compile_pool: depth = number of logical CPUs

if (CMAKE_GENERATOR MATCHES "Ninja")
  # ProcessorCount provides ProcessorCount(<VAR>)
  include(ProcessorCount)
  ProcessorCount(_PROC_COUNT)
  if(NOT DEFINED _PROC_COUNT OR _PROC_COUNT LESS 1)
    set(_PROC_COUNT 1)
  endif()

  # Detect total memory (bytes). Prefer Linux /proc/meminfo, fallback to sysctl on macOS.
  set(_MEM_BYTES 0)
  if(EXISTS "/proc/meminfo")
    file(READ "/proc/meminfo" _meminfo)
    string(REGEX MATCH "MemTotal:[ \t]*([0-9]+) kB" _match "${_meminfo}")
    if(CMAKE_MATCH_1)
      math(EXPR _mem_kb "${CMAKE_MATCH_1}")
      math(EXPR _MEM_BYTES "${_mem_kb} * 1024")
    endif()
  elseif(APPLE)
    execute_process(COMMAND sysctl -n hw.memsize
                    OUTPUT_VARIABLE _sys_mem
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
    if(_sys_mem)
      string(REGEX REPLACE "[^0-9]" "" _sys_mem_clean "${_sys_mem}")
      if(_sys_mem_clean)
        set(_MEM_BYTES ${_sys_mem_clean})
      endif()
    endif()
  endif()

  # If detection failed, assume 8 GiB as a safe default.
  if(NOT _MEM_BYTES OR _MEM_BYTES EQUAL 0)
    set(_MEM_BYTES 8589934592)
  endif()

  # Compute total GB (integer division)
  math(EXPR _MEM_GB "${_MEM_BYTES} / 1073741824")
  if(_MEM_GB LESS 1)
    set(_MEM_GB 1)
  endif()

  # link_pool depth = floor(total_gb / 3), minimum 1
  math(EXPR _LINK_POOL "${_MEM_GB} / 3")
  if(_LINK_POOL LESS 1)
    set(_LINK_POOL 1)
  endif()

  # compile_pool = logical cpu count
  set(_COMPILE_POOL ${_PROC_COUNT})

  message(STATUS "JobPool: detected ${_PROC_COUNT} logical CPUs, ${_MEM_GB} GiB RAM -> link_pool=${_LINK_POOL}, compile_pool=${_COMPILE_POOL}")

  # Export JOB_POOLS global property for generator to use
  set_property(GLOBAL PROPERTY JOB_POOLS link_pool=${_LINK_POOL} compile_pool=${_COMPILE_POOL})
endif()
