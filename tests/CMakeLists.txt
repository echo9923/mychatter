cmake_minimum_required(VERSION 3.25)

# ---------------------------------------------------------------------------
# IM tests (plan Verification.1 / Verification.2)
#
# No test framework: every executable is a plain main() that prints
# [PASS]/[FAIL] per assertion and exits non-zero on any failure.
#
# worker_pool_tests is always registered. It compiles the production
# LogicWorker.cpp (ChatServer) and HandlerExecutor.cpp (GateServer) directly
# and links nothing else from either server, so it runs on any build without
# Redis / MySQL / gRPC / Boost.
#
# im_integration_tests (Verification.2) requires live Redis + MySQL and spawned
# server processes; it is only registered when LLFC_RUN_INTEGRATION_TESTS=ON
# and only once its harness source exists, so the default full-suite stays green.
# ---------------------------------------------------------------------------

add_executable(worker_pool_tests
  worker_pool_tests.cpp
  ../server/ChatServer/src/LogicWorker.cpp
  ../server/GateServer/src/HandlerExecutor.cpp
)

target_include_directories(worker_pool_tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../server/ChatServer/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../server/GateServer/include"
)

target_compile_features(worker_pool_tests PRIVATE cxx_std_17)
llfc_apply_msvc_defaults(worker_pool_tests)

add_test(NAME worker_pool_tests COMMAND worker_pool_tests)

# ---------------------------------------------------------------------------
# Integration tests — guarded placeholder (implemented by Verification.2).
# ---------------------------------------------------------------------------
if(LLFC_RUN_INTEGRATION_TESTS)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/im_integration_tests.cpp")
    # Registered by Verification.2 once the headless harness lands:
    #   add_executable(im_integration_tests im_integration_tests.cpp ...)
    #   target_link_libraries(im_integration_tests PRIVATE
    #     llfc_chat_proto Boost::asio Boost::beast hiredis::hiredis
    #     mysql::concpp-jdbc-static nlohmann_json::nlohmann_json)
    #   add_test(NAME im_integration_tests COMMAND im_integration_tests)
    message(STATUS "tests: im_integration_tests.cpp present; integration registration deferred to Verification.2")
  else()
    message(STATUS "tests: LLFC_RUN_INTEGRATION_TESTS=ON but im_integration_tests.cpp is absent (Verification.2); only worker_pool_tests registered")
  endif()
endif()
