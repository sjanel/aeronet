add_project_executable(aeronet-minimal minimal.cpp)
target_link_libraries(aeronet-minimal PRIVATE aeronet)

add_project_executable(aeronet-multi multi.cpp)
target_link_libraries(aeronet-multi PRIVATE aeronet)

add_project_executable(aeronet-async async.cpp)
target_link_libraries(aeronet-async PRIVATE aeronet)

add_project_executable(aeronet-sendfile sendfile.cpp)
target_link_libraries(aeronet-sendfile PRIVATE aeronet)

add_project_executable(aeronet-static-example static-file.cpp)
target_link_libraries(aeronet-static-example PRIVATE aeronet)
