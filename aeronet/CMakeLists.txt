add_subdirectory(tech)
add_subdirectory(sys)
add_subdirectory(objects)
add_subdirectory(http)
add_subdirectory(main)

if(AERONET_BUILD_TESTS OR AERONET_BUILD_BENCHMARKS)
  add_subdirectory(test_support)
endif()