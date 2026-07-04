add_executable(wikore_tests
    test_org_tree.cpp
    test_access.cpp
    test_auth.cpp
    test_error_mapper.cpp
    test_promote_version.cpp
    test_partition_maintainer.cpp
    test_embedder.cpp
    test_vector_store.cpp
    test_chunker.cpp
    test_parser.cpp
    test_ingest_use_case.cpp
    test_iter1_worker.cpp
    test_retrieval_invariants.cpp
    test_access_resolver.cpp
    test_qdrant_filter_builder.cpp
    test_evidence_gate.cpp
    test_clearance.cpp
    test_retrieval_orchestrator.cpp
    test_resync_worker.cpp
    test_wiki_query_handler.cpp
    test_me_handler.cpp
    test_orgs_tree_handler.cpp
    test_org_get_handler.cpp
    test_allowed_evidence.cpp
    test_llm_provider.cpp
    test_llm_gate.cpp
)
target_link_libraries(wikore_tests PRIVATE
    wikore_ingest_lib
    wikore_scheduler_lib
    wikore_api_handlers
    wikore_core
    wikore_app
    wikore_infra
    Catch2::Catch2WithMain
    wikore_warnings
)
target_compile_definitions(wikore_tests PRIVATE
    TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)
target_include_directories(wikore_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"  # enables #include "support/..."
)
include(Catch)
catch_discover_tests(wikore_tests)
