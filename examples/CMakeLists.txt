add_project_executable(aeronet-minimal minimal.cpp)
target_link_libraries(aeronet-minimal PRIVATE aeronet)

add_project_executable(aeronet-multi multi.cpp)
target_link_libraries(aeronet-multi PRIVATE aeronet)

add_project_executable(aeronet-async async.cpp)
target_link_libraries(aeronet-async PRIVATE aeronet)
