add_subdirectory(basic)
add_subdirectory(full)

if(AERONET_ENABLE_PROTOBUF)
  add_subdirectory(protobuf)
endif()
