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
# Integration tests — headless client + harness (Verification.2-3 first half).
#
# Built and registered to CTest only when LLFC_RUN_INTEGRATION_TESTS=ON. Each
# scenario is its own add_test(... --scenario X) with LABELS "integration" so
# `ctest -L integration` runs exactly the IM scenarios. The default full-suite
# build (option OFF) never pulls in live Redis/MySQL/gRPC.
# ---------------------------------------------------------------------------
if(LLFC_RUN_INTEGRATION_TESTS)
  # Discover the third-party targets the headless client links against. These
  # are not found at the root (only the llfc_find_* helpers are defined there),
  # so each server calls its own find_package; the test does the same here.
  find_package(Boost CONFIG REQUIRED COMPONENTS asio beast)
  find_package(nlohmann_json CONFIG REQUIRED)
  find_package(hiredis CONFIG REQUIRED)
  find_package(mysql-concpp CONFIG REQUIRED)
  # Status gRPC client (im_status_client) drives GetChatServer against the
  # harness StatusServer; mirror StatusServer's proto + gRPC linkage.
  include(GrpcCodegen)
  find_package(gRPC CONFIG REQUIRED)
  find_package(Protobuf CONFIG REQUIRED)
  llfc_ensure_status_proto()

  add_executable(im_integration_tests
    im_integration_tests.cpp
    integration/im_tcp_client.cpp
    integration/im_http_client.cpp
    integration/im_redis.cpp
    integration/im_mysql.cpp
    integration/im_harness.cpp
    integration/im_scenarios.cpp
    integration/im_status_client.cpp
    integration/im_common.h
    integration/im_frame.h
    integration/im_tcp_client.h
    integration/im_http_client.h
    integration/im_redis.h
    integration/im_mysql.h
    integration/im_harness.h
    integration/im_scenarios.h
    integration/im_status_client.h
  )

  target_include_directories(im_integration_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
  )

  target_link_libraries(im_integration_tests PRIVATE
    Boost::asio
    Boost::beast
    nlohmann_json::nlohmann_json
    hiredis::hiredis
    mysql::concpp-jdbc-static
    llfc_status_proto
    gRPC::grpc++
  )

  target_compile_features(im_integration_tests PRIVATE cxx_std_17)
  llfc_apply_msvc_defaults(im_integration_tests)

  # Resolve built server exes at runtime: repo root + build config. The servers
  # are deployed by llfc_set_runtime_outdir into out/run/<config>/<subdir>/. A
  # Ninja Multi-Config Debug build of this target resolves $<CONFIG> to "Debug",
  # matching the servers built with --config Debug.
  target_compile_definitions(im_integration_tests PRIVATE
    IM_REPO_ROOT="${LLFC_REPO_ROOT}"
    IM_BUILD_CONFIG="$<CONFIG>"
    # Target Win7+ for Boost.Asio (silences the _WIN32_WINNT assumption warning)
    # and keep the Win32 headers lean / macro-free for the harness + hiredis.
    _WIN32_WINNT=0x0601
    WIN32_LEAN_AND_MEAN
    NOMINMAX
  )

  # One CTest entry per scenario (gtest_discover-style split), all labelled
  # "integration" so `ctest -L integration` selects exactly these.
  set(_im_scenarios
    gate-smoke order-n4 order-n1 dedup
    offline lost-ack pull-bytes cross-server image-offline
    status-discovery
  )
  foreach(_scn ${_im_scenarios})
    add_test(NAME im_integration_${_scn}
      COMMAND im_integration_tests --scenario ${_scn})
    # All scenarios bind the same fixed harness ports (18080/18090/15052), so
    # they must never run concurrently even under `ctest -j`.
    set_tests_properties(im_integration_${_scn} PROPERTIES
      LABELS "integration" RUN_SERIAL TRUE)
  endforeach()
endif()
