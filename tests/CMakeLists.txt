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

# password_hash_tests needs libcrypto. Found at top level (not inside the
# LLFC_RUN_INTEGRATION_TESTS block) so the default full-suite still covers the
# hash helper. OpenSSL ships transitively via grpc in vcpkg_installed, and is
# declared directly in vcpkg.json.
find_package(OpenSSL REQUIRED)

add_executable(worker_pool_tests
  worker_pool_tests.cpp
  ../server/common/src/LogicWorker.cpp
  ../server/GateServer/src/HandlerExecutor.cpp
)

target_include_directories(worker_pool_tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../server/common/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../server/GateServer/include"
)

target_compile_features(worker_pool_tests PRIVATE cxx_std_17)
llfc_apply_msvc_defaults(worker_pool_tests)

add_test(NAME worker_pool_tests COMMAND worker_pool_tests)

# --- Shared session lifecycle state machine ---------------------------------
add_executable(session_lifecycle_tests
  session_lifecycle_tests.cpp
)

target_include_directories(session_lifecycle_tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../server/common/include"
)

target_link_libraries(session_lifecycle_tests PRIVATE
  llfc_server_common
)

target_compile_features(session_lifecycle_tests PRIVATE cxx_std_17)
llfc_apply_msvc_defaults(session_lifecycle_tests)

add_test(NAME session_lifecycle_tests COMMAND session_lifecycle_tests)

# --- Password hash helper tests ----------------------------------------------
# Compiles the production PasswordHash.cpp directly and links only libcrypto
# (mirrors worker_pool_tests), so it runs on any build without externals.
add_executable(password_hash_tests
  password_hash_tests.cpp
  ../server/common/src/PasswordHash.cpp
)

target_include_directories(password_hash_tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../server/common/include"
)

target_link_libraries(password_hash_tests PRIVATE
  OpenSSL::Crypto
)

target_compile_features(password_hash_tests PRIVATE cxx_std_17)
llfc_apply_msvc_defaults(password_hash_tests)

add_test(NAME password_hash_tests COMMAND password_hash_tests)

# --- Generic SHA-256 helper tests ---------------------------------------------
# Compiles the production Sha256.cpp directly and links only libcrypto
# (mirrors password_hash_tests): FIPS test vectors, hex validation, and the
# streaming file hash against the in-memory hash over ~3MB of binary data.
add_executable(sha256_tests
  sha256_tests.cpp
  ../server/common/src/Sha256.cpp
)

target_include_directories(sha256_tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../server/common/include"
)

target_link_libraries(sha256_tests PRIVATE
  OpenSSL::Crypto
)

target_compile_features(sha256_tests PRIVATE cxx_std_17)
llfc_apply_msvc_defaults(sha256_tests)

add_test(NAME sha256_tests COMMAND sha256_tests)

# --- Client local store (SQLite) tests ---------------------------------------
# Compiles the client's production LocalChatDb sources directly (mirrors
# worker_pool_tests). localchatdb.cpp includes the client's global.h, which
# drags in Qt Widgets/Network headers and needs global.cpp for
# calculateFileHash, so the Qt module set matches the client's (minus WinMain).
# Qt5 is static in the vcpkg triplet, so the QSQLITE driver is pulled in via
# qt5_import_plugins exactly like the llfcchat client target.
llfc_find_client_dependencies()

add_executable(local_store_tests
  local_store_tests.cpp
  ../client/llfcchat/src/store/localchatdb.cpp
  ../client/llfcchat/src/protocol/global.cpp
)

target_include_directories(local_store_tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../client/llfcchat/src/app"
  "${CMAKE_CURRENT_SOURCE_DIR}/../client/llfcchat/src/dialogs"
  "${CMAKE_CURRENT_SOURCE_DIR}/../client/llfcchat/src/chat"
  "${CMAKE_CURRENT_SOURCE_DIR}/../client/llfcchat/src/contact"
  "${CMAKE_CURRENT_SOURCE_DIR}/../client/llfcchat/src/widgets"
  "${CMAKE_CURRENT_SOURCE_DIR}/../client/llfcchat/src/net"
  "${CMAKE_CURRENT_SOURCE_DIR}/../client/llfcchat/src/store"
  "${CMAKE_CURRENT_SOURCE_DIR}/../client/llfcchat/src/protocol"
  "${CMAKE_CURRENT_SOURCE_DIR}/../client/llfcchat/src/util"
  "${LLFC_REPO_ROOT}/proto"
)

target_link_libraries(local_store_tests PRIVATE
  Qt5::Core
  Qt5::Gui
  Qt5::Network
  Qt5::Sql
  Qt5::Widgets
)

# Client code (and the test itself) is C++11 — pin it so that stays enforced.
set_target_properties(local_store_tests PROPERTIES
  CXX_STANDARD 11
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

qt5_import_plugins(local_store_tests
  INCLUDE_BY_TYPE sqldrivers Qt5::QSQLiteDriverPlugin
)

llfc_apply_msvc_defaults(local_store_tests)

add_test(NAME local_store_tests COMMAND local_store_tests)

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
  find_package(Boost CONFIG REQUIRED COMPONENTS asio beast filesystem)
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
    # 资源消息分片/整文件 SHA-256 校验：直接复用生产实现
    ../server/common/src/Sha256.cpp
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
    "${CMAKE_CURRENT_SOURCE_DIR}/../server/common/include"
    "${LLFC_REPO_ROOT}/proto"
  )

  target_link_libraries(im_integration_tests PRIVATE
    Boost::asio
    Boost::beast
    Boost::filesystem
    nlohmann_json::nlohmann_json
    hiredis::hiredis
    mysql::concpp-jdbc-static
    llfc_status_proto
    gRPC::grpc++
    OpenSSL::Crypto
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
    gate-smoke order-n4 order-n1 dedup friend-workflow
    offline pull-bytes cross-server resource-offline
    status-discovery chat-failover simple-auth
    sync-bootstrap big-ids
    resource-create resource-upload resource-resume resource-idempotent
    resource-corrupt resource-perm resource-offset resource-expiry
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
