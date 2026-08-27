# ush_lexer: tokenizer, §2.2/§2.3/§2.10.1. Its own target so tests can link
# it directly without pulling in the rest of the shell (which doesn't exist
# yet).
add_library(ush_lexer STATIC
    lexer/lexer.cpp
    lexer/token.cpp
)
target_include_directories(ush_lexer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(ush_lexer PUBLIC cxx_std_20)

# ush_parser: AST (header-only) + Parser, §2.10.2. Depends on ush_lexer for
# Token/Lexer.
add_library(ush_parser STATIC
    parser/parser.cpp
)
target_include_directories(ush_parser PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ush_parser PUBLIC ush_lexer)
target_compile_features(ush_parser PUBLIC cxx_std_20)

# ush_runtime: shell variable/parameter state (§2.5), used by expansion
# and (later) execution.
add_library(ush_runtime STATIC
    runtime/environment.cpp
)
target_include_directories(ush_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(ush_runtime PUBLIC cxx_std_20)

# ush_expand: word expansion (§2.6) - arithmetic, tilde, parameter, and
# command substitution (the last via an injected CommandRunner interface,
# so this doesn't need to depend on the executor). Field splitting and
# pathname expansion land here next.
add_library(ush_expand STATIC
    expand/arithmetic.cpp
    expand/pattern.cpp
    expand/expander.cpp
    expand/field_split.cpp
    expand/pathname_expand.cpp
)
target_include_directories(ush_expand PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ush_expand PUBLIC ush_runtime ush_lexer)
target_compile_features(ush_expand PUBLIC cxx_std_20)

add_executable(ush
    main.cpp
)
target_link_libraries(ush PRIVATE ush_parser ush_expand)
