add_executable(ush_tests
    lexer/test_lexer.cpp
    parser/test_parser.cpp
    runtime/test_environment.cpp
    expand/test_arithmetic.cpp
    expand/test_expander.cpp
)
target_link_libraries(ush_tests PRIVATE ush_parser ush_expand Catch2::Catch2WithMain)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(ush_tests)
