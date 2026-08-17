cmake_minimum_required(VERSION 3.25)

# Standalone entry when configuring directly from server/common/
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  if(NOT CMAKE_GENERATOR STREQUAL "Ninja Multi-Config")
    message(FATAL_ERROR
      "This project requires the \"Ninja Multi-Config\" generator. "
      "Configure with: cmake -G \"Ninja Multi-Config\" ...")
  endif()
  if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are not supported. Use a separate build directory.")
  endif()
  get_filename_component(_llfc_repo_root "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
  set(VCPKG_MANIFEST_DIR "${_llfc_repo_root}" CACHE PATH "vcpkg manifest directory" FORCE)
  set(VCPKG_INSTALLED_DIR "${_llfc_repo_root}/vcpkg_installed" CACHE PATH "vcpkg installed directory" FORCE)
  set(LLFC_REPO_ROOT "${_llfc_repo_root}" CACHE PATH "llfcchat repository root" FORCE)

  project(llfc_server_common LANGUAGES CXX)

  list(APPEND CMAKE_MODULE_PATH "${LLFC_REPO_ROOT}/cmake")
  include(Dependencies)
  include(GrpcCodegen)

  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
endif()

if(NOT DEFINED LLFC_REPO_ROOT)
  get_filename_component(LLFC_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
  set(LLFC_REPO_ROOT "${LLFC_REPO_ROOT}" CACHE PATH "llfcchat repository root" FORCE)
endif()

# Narrow find_package subset: only what the common sources actually need.
# Deliberately does NOT pull gRPC / Protobuf / nlohmann_json — those are
# service-level concerns. (llfc_find_server_dependencies() would also work —
# it is include_guard(GLOBAL) and idempotent — but would over-specify the
# common lib's requirements and couple it to gRPC for no reason.)
find_package(Boost CONFIG REQUIRED COMPONENTS asio filesystem property_tree uuid)
find_package(hiredis CONFIG REQUIRED)
find_package(mysql-concpp CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

# --- Base common lib: config + asio pool + logic worker ----------------------
add_library(llfc_server_common STATIC
  src/ConfigMgr.cpp
  src/AsioIOServicePool.cpp
  src/LogicWorker.cpp
  include/SessionLifecycle.h
)
target_include_directories(llfc_server_common PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(llfc_server_common PUBLIC
  Boost::asio
  Boost::filesystem
  Boost::property_tree
)
target_compile_features(llfc_server_common PUBLIC cxx_std_17)
llfc_apply_msvc_defaults(llfc_server_common)

# --- Redis layer: pooled hiredis client + distributed lock -------------------
add_library(llfc_server_common_redis STATIC
  src/RedisConPool.cpp
  src/DistLock.cpp
)
target_link_libraries(llfc_server_common_redis PUBLIC
  llfc_server_common
  hiredis::hiredis
  Boost::uuid
)
target_compile_features(llfc_server_common_redis PUBLIC cxx_std_17)
llfc_apply_msvc_defaults(llfc_server_common_redis)

# --- MySQL layer: pooled mysql-concpp client ---------------------------------
add_library(llfc_server_common_mysql STATIC
  src/MySqlPool.cpp
)
target_link_libraries(llfc_server_common_mysql PUBLIC
  llfc_server_common
  mysql::concpp-jdbc-static
)
target_compile_features(llfc_server_common_mysql PUBLIC cxx_std_17)
llfc_apply_msvc_defaults(llfc_server_common_mysql)

# --- Crypto layer: PBKDF2 password hashing + generic SHA-256 (libcrypto) ------
# Only services that hash passwords or verify resource integrity link this
# (GateServer, ChatServer, ResourceServer, and the unit tests). Kept out of
# llfc_server_common so the base lib stays dependency-lean. find_package is
# idempotent: llfc_find_server_dependencies() re-finds it for the servers.
add_library(llfc_server_common_crypto STATIC
  src/PasswordHash.cpp
  src/Sha256.cpp
)
target_include_directories(llfc_server_common_crypto PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(llfc_server_common_crypto PUBLIC
  OpenSSL::Crypto
)
target_compile_features(llfc_server_common_crypto PUBLIC cxx_std_17)
llfc_apply_msvc_defaults(llfc_server_common_crypto)
