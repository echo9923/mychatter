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

  project(ChatServer LANGUAGES CXX)

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
llfc_ensure_status_proto()

add_executable(ChatServer
  src/ChatGrpcClient.cpp
  src/ChatServer.cpp
  src/ChatServiceImpl.cpp
  src/CServer.cpp
  src/CSession.cpp
  src/LogicSystem.cpp
  src/MsgNode.cpp
  src/MysqlDao.cpp
  src/MysqlMgr.cpp
  src/RedisMgr.cpp
  src/UserMgr.cpp
  src/utils.cpp
  include/ChatGrpcClient.h
  include/ChatServiceImpl.h
  include/const.h
  include/CServer.h
  include/CSession.h
  include/data.h
  include/LogicSystem.h
  include/MsgNode.h
  include/MysqlDao.h
  include/MysqlMgr.h
  include/RedisMgr.h
  include/UserMgr.h
  include/utils.h
)

target_include_directories(ChatServer PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(ChatServer PRIVATE
  llfc_server_common
  llfc_server_common_crypto
  llfc_server_common_redis
  llfc_server_common_mysql
  llfc_chat_proto
  llfc_status_proto
  Boost::beast
  Boost::date_time
  nlohmann_json::nlohmann_json
)

target_compile_features(ChatServer PRIVATE cxx_std_17)
llfc_apply_msvc_defaults(ChatServer)
llfc_set_runtime_outdir(ChatServer chatserver1)

add_custom_command(TARGET ChatServer POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/config/chatserver1.ini"
    "$<TARGET_FILE_DIR:ChatServer>/config.ini"
  COMMAND "${CMAKE_COMMAND}" -E make_directory
    "$<TARGET_FILE_DIR:ChatServer>/../chatserver2"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "$<TARGET_FILE:ChatServer>"
    "$<TARGET_FILE_DIR:ChatServer>/../chatserver2/$<TARGET_FILE_NAME:ChatServer>"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/config/chatserver2.ini"
    "$<TARGET_FILE_DIR:ChatServer>/../chatserver2/config.ini"
  COMMENT "Deploy ChatServer configs and chatserver2 instance"
  VERBATIM
)
