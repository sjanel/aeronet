aux_source_directory(src AERONET_WEB_SRC_LIST)

AeronetAddProjectLibrary(aeronet_web
    ${AERONET_WEB_SRC_LIST}
)

target_include_directories(aeronet_web
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        # The internal headers of aeronet_html: the DOM bindings work on the lexbor nodes of its documents.
        ${PROJECT_SOURCE_DIR}/aeronet/html/src
)

# html and client are public (WebPage::document(), WebClient::httpClient()). QuickJS-ng stays private. lexbor is only
# compiled with, for the layout of its nodes: aeronet_html is the only library that calls it, so that a shared build
# has a single copy of it.
target_link_libraries(aeronet_web
    PUBLIC
        aeronet_html
        aeronet_client
    PRIVATE
        qjs
        $<COMPILE_ONLY:${AERONET_LEXBOR_TARGET}>
)

# --- Tests ---

# The engine wrapper, compiled with its own QuickJS-ng: aeronet_web hides the one it links.
AeronetAddUnitTest(
    web-engine_test
    test/web-engine_test.cpp
    src/js-engine.cpp
    LIBRARIES
    qjs
    aeronet_tech
    aeronet_basic_test_support
)

# The URL of the documents (DocumentUrl), compiled with its source: no need of the runtime of the pages.
AeronetAddUnitTest(
    web-url_test
    test/web-url_test.cpp
    src/document-url.cpp
    LIBRARIES
    aeronet_tech
    aeronet_basic_test_support
)

# Their targets are configured below: only with the tests.
if(AERONET_ENABLE_HTTP_SERVER AND AERONET_BUILD_TESTS)
    foreach(_web_test IN ITEMS web-address-space_test web-app_test web-client_test web-dom_test web-network_test web-runtime_test)
        AeronetAddUnitTest(
            ${_web_test}
            test/${_web_test}.cpp
            LIBRARIES
            aeronet_web
            aeronet
            aeronet_test_support
        )
        if(MSVC)
            # The main thread of a Windows executable has 1 MiB of stack, the default maxScriptStackSize: an endless
            # recursion would overflow it before the RangeError. QuickJS-ng links its own executables with 8 MiB too.
            target_link_options(${_web_test} PRIVATE /STACK:8388608)
        endif()
    endforeach()
    # The vendored production builds of React served to the pages of the tests.
    target_compile_definitions(web-app_test PRIVATE AERONET_WEB_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test/data")
endif()
