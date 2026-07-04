# HTTP handlers, in a static lib so the test suite can drive them directly
# (the wikore_api executable is not linkable from tests). handlers.hpp is
# exposed via the PUBLIC include dir so consumers can `#include "handlers.hpp"`.
add_library(wikore_api_handlers STATIC
    wiki_query.cpp
    me.cpp
)
target_include_directories(wikore_api_handlers PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(wikore_api_handlers PUBLIC
    wikore_core
    wikore_rag
    PRIVATE
    wikore_warnings
)

add_executable(wikore_api
    main.cpp
)

target_link_libraries(wikore_api PRIVATE
    wikore_api_handlers
    wikore_core
    wikore_rag
    wikore_wiki
    wikore_mcp
    wikore_warnings
)

# Git hash baked in at link time
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE WIKORE_GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
target_compile_definitions(wikore_api PRIVATE
    WIKORE_GIT_HASH="${WIKORE_GIT_HASH}"
)
