cmake_minimum_required(VERSION 3.25)

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

  project(ResourceServer LANGUAGES CXX)

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
llfc_ensure_chat_proto()

add_executable(ResourceServer
  src/base64.cpp
  src/ChatServerGrpcClient.cpp
  src/ConfigMgr.cpp
  src/CServer.cpp
  src/CSession.cpp
  src/FileInfo.cpp
  src/FileSystem.cpp
  src/FileWorker.cpp
  src/LogicSystem.cpp
  src/LogicWorker.cpp
  src/MsgNode.cpp
  src/MysqlDao.cpp
  src/MysqlMgr.cpp
  src/RedisMgr.cpp
  src/ResourceServer.cpp
  src/UserMgr.cpp
  include/base64.h
  include/ChatServerGrpcClient.h
  include/ConfigMgr.h
  include/const.h
  include/CServer.h
  include/CSession.h
  include/data.h
  include/FileInfo.h
  include/FileSystem.h
  include/FileWorker.h
  include/LogicSystem.h
  include/LogicWorker.h
  include/MsgNode.h
  include/MysqlDao.h
  include/MysqlMgr.h
  include/RedisMgr.h
  include/UserMgr.h
)

target_include_directories(ResourceServer PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(ResourceServer PRIVATE
  llfc_server_common
  llfc_server_common_crypto
  llfc_server_common_redis
  llfc_server_common_mysql
  llfc_chat_proto
  Boost::beast
  nlohmann_json::nlohmann_json
)

target_compile_features(ResourceServer PRIVATE cxx_std_17)
llfc_apply_msvc_defaults(ResourceServer)
llfc_set_runtime_outdir(ResourceServer ResourceServer)

add_custom_command(TARGET ResourceServer POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/config/config.ini"
    "$<TARGET_FILE_DIR:ResourceServer>/config.ini"
  COMMENT "Deploy ResourceServer config.ini"
  VERBATIM
)
