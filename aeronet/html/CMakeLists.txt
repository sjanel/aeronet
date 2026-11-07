aux_source_directory(src AERONET_HTML_SRC_LIST)

AeronetAddProjectLibrary(aeronet_html
    ${AERONET_HTML_SRC_LIST}
)

# Public includes
target_include_directories(aeronet_html
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# tech (vector) is public. lexbor stays private: the public headers only forward-declare its types.
# objects: the parameters of the Content-Type header (charset).
target_link_libraries(aeronet_html
    PUBLIC
        aeronet_tech
    PRIVATE
        aeronet_objects
        ${AERONET_LEXBOR_TARGET}
)

# --- Tests ---

AeronetAddUnitTest(
    assigned-json_test
    test/assigned-json_test.cpp
    src/assigned-json.cpp
)

AeronetAddUnitTest(
    css-selector_test
    test/css-selector_test.cpp
    LIBRARIES
    aeronet_html
    aeronet_basic_test_support
)

AeronetAddUnitTest(
    html-document_test
    test/html-document_test.cpp
    LIBRARIES
    aeronet_html
    aeronet_basic_test_support
)

AeronetAddUnitTest(
    html-element_test
    test/html-element_test.cpp
    LIBRARIES
    aeronet_html
    aeronet_basic_test_support
)
