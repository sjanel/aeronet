add_executable(aeronet-minimal minimal.cpp)

target_link_libraries(aeronet-minimal PRIVATE aeronet)

add_executable(aeronet-multi multi.cpp)
target_link_libraries(aeronet-multi PRIVATE aeronet)
