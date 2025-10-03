vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO AmadeusITGroup/amc
    REF v2.6.1
    SHA512 6a944e8234420afebe5fb07c49aa07225c111f7d11426a279011d2aa29a36cd171cd999ee0dd3ebba544e1ef31918be69ecb8b8bc63072dda4fbcfc5b8c38d0e
)

# Install headers (best effort)
if(EXISTS "${SOURCE_PATH}/include")
  file(INSTALL "${SOURCE_PATH}/include" DESTINATION "${CURRENT_PACKAGES_DIR}")
else()
  message(FATAL_ERROR "[amc] Expected include directory not found in fetched sources: ${SOURCE_PATH}")
endif()

# Attempt to install license file
foreach(LIC IN ITEMS LICENSE LICENSE.txt LICENSE.md COPYING)
  if(EXISTS "${SOURCE_PATH}/${LIC}")
    file(INSTALL "${SOURCE_PATH}/${LIC}" DESTINATION "${CURRENT_PACKAGES_DIR}/share/amc" RENAME copyright)
    set(AMC_LICENSE_DONE TRUE)
    break()
  endif()
endforeach()
if(NOT AMC_LICENSE_DONE)
  file(WRITE "${CURRENT_PACKAGES_DIR}/share/amc/copyright" "See upstream repository for license.")
endif()

# Generate a lightweight CMake config with an interface target without hard-coding
# absolute paths into the file (vcpkg post-build check warns otherwise). We compute
# the absolute prefix at *consumer* configure time from CMAKE_CURRENT_LIST_DIR.
file(MAKE_DIRECTORY "${CURRENT_PACKAGES_DIR}/share/amc")
set(AMC_CONFIG_FILE "${CURRENT_PACKAGES_DIR}/share/amc/amcConfig.cmake")
file(WRITE ${AMC_CONFIG_FILE} "# Auto-generated minimalist config for amc\n")
file(APPEND ${AMC_CONFIG_FILE} "if(NOT TARGET amc::amc)\n")
file(APPEND ${AMC_CONFIG_FILE} "  add_library(amc::amc INTERFACE IMPORTED)\n")
file(APPEND ${AMC_CONFIG_FILE} "  # Compute absolute include directory relative to this config file at consumer time\n")
file(APPEND ${AMC_CONFIG_FILE} "  get_filename_component(_amc_include_dir \"\${CMAKE_CURRENT_LIST_DIR}/../../include\" ABSOLUTE)\n")
file(APPEND ${AMC_CONFIG_FILE} "  set_target_properties(amc::amc PROPERTIES INTERFACE_INCLUDE_DIRECTORIES \"\${_amc_include_dir}\")\n")
file(APPEND ${AMC_CONFIG_FILE} "  unset(_amc_include_dir)\n")
file(APPEND ${AMC_CONFIG_FILE} "endif()\n")
