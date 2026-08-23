cmake_minimum_required(VERSION 3.25)

# Standalone entry when configuring from server/GateServer
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

  project(GateServer LANGUAGES CXX)

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

llfc_find_server_dependencies()
llfc_ensure_status_proto()
llfc_ensure_verify_proto()

add_executable(GateServer
  src/CServer.cpp
  src/GateServer.cpp
  src/HandlerExecutor.cpp
  src/HttpConnection.cpp
  src/LogicSystem.cpp
  src/MysqlDao.cpp
  src/MysqlMgr.cpp
  src/RedisMgr.cpp
  src/StatusGrpcClient.cpp
  src/VerifyGrpcClient.cpp
  include/const.h
  include/CServer.h
  include/HandlerExecutor.h
  include/HttpConnection.h
  include/LogicSystem.h
  include/MysqlDao.h
  include/MysqlMgr.h
  include/RedisMgr.h
  include/StatusGrpcClient.h
  include/VerifyGrpcClient.h
)

target_include_directories(GateServer PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${LLFC_REPO_ROOT}/proto"
)

target_link_libraries(GateServer PRIVATE
  llfc_server_common
  llfc_server_common_crypto
  llfc_server_common_redis
  llfc_server_common_mysql
  llfc_status_proto
  llfc_verify_proto
  Boost::beast
  nlohmann_json::nlohmann_json
)

target_compile_features(GateServer PRIVATE cxx_std_17)
llfc_apply_msvc_defaults(GateServer)
llfc_set_runtime_outdir(GateServer GateServer)

add_custom_command(TARGET GateServer POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/config/config.ini"
    "$<TARGET_FILE_DIR:GateServer>/config.ini"
  COMMENT "Deploy GateServer config.ini"
  VERBATIM
)
