cmake_minimum_required(VERSION 3.25)

# Standalone / root entry checks and vcpkg dirs (must run before project()).
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  if(NOT CMAKE_GENERATOR STREQUAL "Ninja Multi-Config")
    message(FATAL_ERROR
      "This project requires the \"Ninja Multi-Config\" generator. "
      "Configure with: cmake -G \"Ninja Multi-Config\" ...")
  endif()
  if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are not supported. Use a separate build directory.")
  endif()
  set(VCPKG_MANIFEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}" CACHE PATH "vcpkg manifest directory" FORCE)
  set(VCPKG_INSTALLED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg_installed" CACHE PATH "vcpkg installed directory" FORCE)
  set(LLFC_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}" CACHE PATH "llfcchat repository root" FORCE)
endif()

project(llfcchat LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Dependencies)
include(GrpcCodegen)

if(NOT DEFINED LLFC_REPO_ROOT)
  set(LLFC_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}" CACHE PATH "llfcchat repository root" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# CTest is required so tests/CMakeLists.txt can register add_test() entries.
# Configured before add_subdirectory(tests) so enable_testing() is in scope.
include(CTest)

# Integration tests require live Redis + MySQL and spawned server processes;
# off by default so the full-suite build never fails for missing externals.
# worker_pool_tests is always registered regardless of this option.
option(LLFC_RUN_INTEGRATION_TESTS "Enable integration tests requiring live Redis/MySQL and spawned server processes" OFF)

add_subdirectory(server)
add_subdirectory(client/llfcchat)
add_subdirectory(tests)
