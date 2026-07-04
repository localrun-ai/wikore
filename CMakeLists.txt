cmake_minimum_required(VERSION 3.28)
project(wikore CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_program(CCACHE ccache)
if(CCACHE)
    set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE})
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# ---------------------------------------------------------------------------
# Release flags
# ---------------------------------------------------------------------------
add_compile_options($<$<CONFIG:Release>:-O3>)

option(WIKORE_NATIVE_OPTS "Apply -march=native and -flto=thin in Release" ON)
if(WIKORE_NATIVE_OPTS)
    add_compile_options($<$<CONFIG:Release>:-flto=thin>)
    add_compile_options($<$<CONFIG:Release>:-march=native>)
    add_compile_options($<$<CONFIG:Release>:-fno-plt>)
    add_link_options($<$<CONFIG:Release>:-flto=thin>)
    add_link_options($<$<CONFIG:Release>:-Wl,-z,now,-z,relro>)
endif()

# ---------------------------------------------------------------------------
# Debug flags
# ---------------------------------------------------------------------------
add_compile_options($<$<CONFIG:Debug>:-O0>)
add_compile_options($<$<CONFIG:Debug>:-g>)
add_compile_options($<$<CONFIG:Debug>:-fsanitize=address,undefined>)
add_link_options($<$<CONFIG:Debug>:-fsanitize=address,undefined>)

add_compile_definitions($<$<CONFIG:Debug>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG>)

# ---------------------------------------------------------------------------
# Warning interface target (our code only, not third-party)
# ---------------------------------------------------------------------------
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag(-Wno-missing-designated-field-initializers
    _WNO_MISSING_DESIGNATED_SUPPORTED)

add_library(wikore_warnings INTERFACE)
target_compile_options(wikore_warnings INTERFACE
    -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers
    $<$<BOOL:${_WNO_MISSING_DESIGNATED_SUPPORTED}>:-Wno-missing-designated-field-initializers>
    $<$<CONFIG:Debug>:-Werror>
)

# ---------------------------------------------------------------------------
# System dependencies
# ---------------------------------------------------------------------------
find_package(spdlog REQUIRED)
find_package(OpenSSL REQUIRED)       # JWT RS256 + AES-256-GCM credential encryption

# hiredis (Redis)
find_package(hiredis CONFIG QUIET)
if(NOT hiredis_FOUND)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(HIREDIS REQUIRED IMPORTED_TARGET hiredis)
    add_library(hiredis::hiredis ALIAS PkgConfig::HIREDIS)
endif()

# ---------------------------------------------------------------------------
# Non-text parser dependencies
#   poppler-cpp  — PDF text + outline extraction
#   minizip      — ZIP container for DOCX
#   pugixml      — lightweight XML parser for word/document.xml
#   libxml2      — HTML parser (htmlReadMemory)
# ---------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(POPPLER_CPP REQUIRED IMPORTED_TARGET poppler-cpp)
pkg_check_modules(MINIZIP     REQUIRED IMPORTED_TARGET minizip)
pkg_check_modules(LIBXML2     REQUIRED IMPORTED_TARGET libxml-2.0)
find_package(pugixml REQUIRED)

# ---------------------------------------------------------------------------
# CPM packages
# ---------------------------------------------------------------------------
include(cmake/dependencies.cmake)

# ---------------------------------------------------------------------------
# wikore_infra: foundation adapters (postgres UoW, error mapper)
# ---------------------------------------------------------------------------
add_library(wikore_infra STATIC
    src/adapters/postgres/unit_of_work.cpp
    src/adapters/postgres/error_mapper.cpp
)
target_include_directories(wikore_infra PUBLIC include)
target_link_libraries(wikore_infra PUBLIC
    spdlog::spdlog
    drogon
    PkgConfig::LIBUUID
    wikore_warnings
)

# ---------------------------------------------------------------------------
# wikore_app: application use cases
# ---------------------------------------------------------------------------
add_library(wikore_app STATIC
    src/application/promote_document_version.cpp
)
target_link_libraries(wikore_app PUBLIC wikore_infra)

# ---------------------------------------------------------------------------
# wikore_core: config, auth, DB helpers, org tree, access resolution
# ---------------------------------------------------------------------------
add_library(wikore_core STATIC
    src/config.cpp
    src/auth.cpp
    src/db.cpp
    src/org_tree.cpp
    src/access.cpp
    src/access_resolver.cpp
    src/redis.cpp
)
target_include_directories(wikore_core PUBLIC include)
target_link_libraries(wikore_core PUBLIC
    wikore_infra          # postgres error_mapper (map_db_exception), UnitOfWork
    spdlog::spdlog
    OpenSSL::SSL
    OpenSSL::Crypto
    drogon
    glaze::glaze
    jwt-cpp::jwt-cpp
    wikore_warnings
)
target_link_libraries(wikore_core PRIVATE hiredis::hiredis)

# ---------------------------------------------------------------------------
# wikore_rag: embedder, retriever, reranker, generator, pipeline, llm providers
# ---------------------------------------------------------------------------
add_library(wikore_rag STATIC
    src/rag/embedder.cpp
    src/rag/retriever.cpp
    src/rag/reranker.cpp
    src/rag/generator.cpp
    src/rag/pipeline.cpp
    src/rag/qdrant_filter_builder.cpp
    src/rag/evidence_gate.cpp
    src/rag/clearance.cpp
    src/rag/retrieval_orchestrator.cpp
    src/rag/llm_gate.cpp
    src/rag/llm_provider_openai.cpp
    src/rag/llm_provider_anthropic.cpp
    src/rag/llm_provider_factory.cpp
)
target_link_libraries(wikore_rag PUBLIC wikore_core)

# ---------------------------------------------------------------------------
# wikore_ingest: parser, chunker, document repo, ingest types, worker
# ---------------------------------------------------------------------------
add_library(wikore_ingest_lib STATIC
    src/ingest/types.cpp
    src/ingest/chunker.cpp
    src/ingest/parser.cpp
    src/ingest/pdf_parser.cpp
    src/ingest/docx_parser.cpp
    src/ingest/pptx_parser.cpp
    src/ingest/odt_parser.cpp
    src/ingest/xlsx_parser.cpp
    src/ingest/html_parser.cpp
    src/ingest/document_repo.cpp
    src/ingest/worker.cpp
    src/application/ingest_document_version.cpp
)
target_link_libraries(wikore_ingest_lib PUBLIC wikore_rag)
target_link_libraries(wikore_ingest_lib PRIVATE
    PkgConfig::POPPLER_CPP
    PkgConfig::MINIZIP
    PkgConfig::LIBXML2
    pugixml::pugixml
)

# ---------------------------------------------------------------------------
# wikore_scheduler_lib: outbox drainer + polling fallback
# ---------------------------------------------------------------------------
add_library(wikore_scheduler_lib STATIC
    src/scheduler/outbox_worker.cpp
    src/scheduler/resync_worker.cpp
    src/scheduler/polling_fallback.cpp
    src/scheduler/partition_maintainer.cpp
)
# wikore_ingest_lib provides DocumentRepoPort::fetch_access_scopes, the
# canonical live scope recompute the resync worker reuses (no duplicated SQL).
target_link_libraries(wikore_scheduler_lib PUBLIC wikore_rag wikore_infra wikore_ingest_lib)

# ---------------------------------------------------------------------------
# wikore_wiki: wiki store + ops (DB-backed, org-scoped)
# ---------------------------------------------------------------------------
add_library(wikore_wiki STATIC
    src/wiki/store.cpp
    src/wiki/ops.cpp
)
target_link_libraries(wikore_wiki PUBLIC wikore_core wikore_rag)

# ---------------------------------------------------------------------------
# wikore_mcp: MCP tool registry + built-in integration adapters
# ---------------------------------------------------------------------------
add_library(wikore_mcp STATIC
    src/mcp/registry.cpp
    src/mcp/slack.cpp
    src/mcp/jira.cpp
    src/mcp/confluence.cpp
    src/mcp/github.cpp
    src/mcp/webhook.cpp
)
target_link_libraries(wikore_mcp PUBLIC wikore_core)

# ---------------------------------------------------------------------------
# Apps
# ---------------------------------------------------------------------------
option(WIKORE_BUILD_APPS "Build all wikore binaries" ON)
if(WIKORE_BUILD_APPS)
    add_subdirectory(apps/api)
    add_subdirectory(apps/ingest)
    add_subdirectory(apps/scheduler)
    add_subdirectory(apps/eval)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
option(WIKORE_BUILD_TESTS "Build Catch2 unit tests" ON)
if(WIKORE_BUILD_TESTS)
    if(Catch2_SOURCE_DIR)
        list(APPEND CMAKE_MODULE_PATH ${Catch2_SOURCE_DIR}/extras)
    endif()
    enable_testing()
    add_subdirectory(tests)
endif()
