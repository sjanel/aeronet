if(AERONET_ENABLE_ASYNC_HANDLERS)
	AeronetAddProjectExecutable(aeronet-async-handlers async-handlers.cpp)
	target_link_libraries(aeronet-async-handlers PRIVATE aeronet_server)
endif()

if(AERONET_ENABLE_HTTP_CLIENT)
	AeronetAddProjectExecutable(aeronet-client-minimal client-minimal.cpp)
	target_link_libraries(aeronet-client-minimal PRIVATE aeronet_client)
endif()

AeronetAddProjectExecutable(aeronet-server-minimal server-minimal.cpp)
target_link_libraries(aeronet-server-minimal PRIVATE aeronet_server)

AeronetAddProjectExecutable(aeronet-multi multi.cpp)
target_link_libraries(aeronet-multi PRIVATE aeronet_server)

AeronetAddProjectExecutable(aeronet-sendfile sendfile.cpp)
target_link_libraries(aeronet-sendfile PRIVATE aeronet_server)

AeronetAddProjectExecutable(aeronet-static-example static-file.cpp)
target_link_libraries(aeronet-static-example PRIVATE aeronet_server)

if (AERONET_ENABLE_OPENSSL)
	AeronetAddProjectExecutable(aeronet-tls-session-tickets tls-session-tickets.cpp)
	target_link_libraries(aeronet-tls-session-tickets PRIVATE aeronet_server)
	
	AeronetAddProjectExecutable(aeronet-tls-ktls tls-ktls.cpp)
	target_link_libraries(aeronet-tls-ktls PRIVATE aeronet_server)
endif()

if(AERONET_ENABLE_HTTP2)
	AeronetAddProjectExecutable(aeronet-http2 http2.cpp)
	target_link_libraries(aeronet-http2 PRIVATE aeronet_server)

	if (AERONET_ENABLE_OPENSSL)
		AeronetAddProjectExecutable(aeronet-static-file-http2 static-file-http2.cpp)
		target_link_libraries(aeronet-static-file-http2 PRIVATE aeronet_server)
	endif()
endif()

if (AERONET_ENABLE_WEBSOCKET)
	AeronetAddProjectExecutable(aeronet-websocket-echo websocket-echo.cpp)
	target_link_libraries(aeronet-websocket-echo PRIVATE aeronet_server)
endif()

if (AERONET_ENABLE_GLAZE)
	AeronetAddProjectExecutable(aeronet-config-dump config-dump.cpp)
	target_link_libraries(aeronet-config-dump PRIVATE aeronet_server)
endif()

