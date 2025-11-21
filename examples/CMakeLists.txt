AeronetAddProjectExecutable(aeronet-minimal minimal.cpp)
target_link_libraries(aeronet-minimal PRIVATE aeronet)

AeronetAddProjectExecutable(aeronet-multi multi.cpp)
target_link_libraries(aeronet-multi PRIVATE aeronet)

AeronetAddProjectExecutable(aeronet-async-handle async-handle.cpp)
target_link_libraries(aeronet-async-handle PRIVATE aeronet)

AeronetAddProjectExecutable(aeronet-sendfile sendfile.cpp)
target_link_libraries(aeronet-sendfile PRIVATE aeronet)

AeronetAddProjectExecutable(aeronet-static-example static-file.cpp)
target_link_libraries(aeronet-static-example PRIVATE aeronet)

if (AERONET_ENABLE_OPENSSL AND AERONET_ENABLE_KTLS)
	AeronetAddProjectExecutable(aeronet-tls-ktls tls-ktls.cpp)
	target_link_libraries(aeronet-tls-ktls PRIVATE aeronet)
endif()
