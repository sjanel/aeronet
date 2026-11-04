if(AERONET_ENABLE_HTTP_SERVER AND AERONET_ENABLE_ASYNC_HANDLERS)
	AeronetAddProjectExecutable(aeronet-async-handlers async-handlers.cpp)
	target_link_libraries(aeronet-async-handlers PRIVATE aeronet_server)
endif()

if(AERONET_ENABLE_HTTP_CLIENT)
	AeronetAddProjectExecutable(aeronet-client-minimal client-minimal.cpp)
	target_link_libraries(aeronet-client-minimal PRIVATE aeronet_client)
endif()

if(AERONET_ENABLE_HTTP_SERVER)
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

	# ProtoJSON needs the descriptors of the full runtime.
	if(AERONET_ENABLE_PROTOBUF AND AERONET_PROTOBUF_RUNTIME STREQUAL "full")
		# Generated code: not a project target, it is compiled without the warnings and clang-tidy of the project.
		add_library(protobuf_shop_messages STATIC)
		protobuf_generate(
			TARGET protobuf_shop_messages
			LANGUAGE cpp
			PROTOS protobuf-shop.proto
			IMPORT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
			PROTOC_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}"
		)
		target_include_directories(protobuf_shop_messages SYSTEM PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")
		target_link_libraries(protobuf_shop_messages PUBLIC ${AERONET_PROTOBUF_TARGET})

		AeronetAddProjectExecutable(aeronet-protobuf protobuf.cpp)
		target_link_libraries(aeronet-protobuf PRIVATE aeronet_server protobuf_shop_messages)
	endif()

	if (AERONET_ENABLE_GLAZE)
		AeronetAddProjectExecutable(aeronet-config-dump config-dump.cpp)
		target_link_libraries(aeronet-config-dump PRIVATE aeronet_server)
		if(AERONET_ENABLE_HTTP_CLIENT)
			target_link_libraries(aeronet-config-dump PRIVATE aeronet_client)
		endif()
	endif()
endif()

