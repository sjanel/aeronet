if(AERONET_ENABLE_ASYNC_HANDLERS)
	AeronetAddProjectExecutable(aeronet-async-handlers async-handlers.cpp)
	target_link_libraries(aeronet-async-handlers PRIVATE aeronet)
endif()

AeronetAddProjectExecutable(aeronet-minimal minimal.cpp)
target_link_libraries(aeronet-minimal PRIVATE aeronet)

AeronetAddProjectExecutable(aeronet-multi multi.cpp)
target_link_libraries(aeronet-multi PRIVATE aeronet)

AeronetAddProjectExecutable(aeronet-sendfile sendfile.cpp)
target_link_libraries(aeronet-sendfile PRIVATE aeronet)

AeronetAddProjectExecutable(aeronet-static-example static-file.cpp)
target_link_libraries(aeronet-static-example PRIVATE aeronet)

if (AERONET_ENABLE_OPENSSL)
	AeronetAddProjectExecutable(aeronet-tls-session-tickets tls-session-tickets.cpp)
	target_link_libraries(aeronet-tls-session-tickets PRIVATE aeronet)
	
	AeronetAddProjectExecutable(aeronet-tls-ktls tls-ktls.cpp)
	target_link_libraries(aeronet-tls-ktls PRIVATE aeronet)
endif()

if(AERONET_ENABLE_HTTP2)
	AeronetAddProjectExecutable(aeronet-http2 http2.cpp)
	target_link_libraries(aeronet-http2 PRIVATE aeronet)

	if (AERONET_ENABLE_OPENSSL)
		AeronetAddProjectExecutable(aeronet-static-file-http2 static-file-http2.cpp)
		target_link_libraries(aeronet-static-file-http2 PRIVATE aeronet)
	endif()
endif()

if (AERONET_ENABLE_WEBSOCKET)
	AeronetAddProjectExecutable(aeronet-websocket-echo websocket-echo.cpp)
	target_link_libraries(aeronet-websocket-echo PRIVATE aeronet)
endif()

