add_executable(ush_tests
    lexer/test_lexer.cpp
    parser/test_parser.cpp
    runtime/test_environment.cpp
    expand/test_arithmetic.cpp
    expand/test_expander.cpp
    expand/test_field_split.cpp
    expand/test_pathname_expand.cpp
    integration/test_shell_integration.cpp
)
target_link_libraries(ush_tests PRIVATE ush_parser ush_expand Catch2::Catch2WithMain)
target_compile_definitions(ush_tests PRIVATE USH_BINARY_PATH="$<TARGET_FILE:ush>")
add_dependencies(ush_tests ush)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(ush_tests)
