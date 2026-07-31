# Shared third-party dependency resolution for all C++ targets.
# Requires vcpkg toolchain + x64-windows-static-md installed tree.

include_guard(GLOBAL)

function(llfc_find_server_dependencies)
  find_package(Boost CONFIG REQUIRED COMPONENTS
    asio
    beast
    date_time
    filesystem
    property_tree
    uuid
  )
  find_package(nlohmann_json CONFIG REQUIRED)
  find_package(gRPC CONFIG REQUIRED)
  find_package(Protobuf CONFIG REQUIRED)
  find_package(hiredis CONFIG REQUIRED)
  find_package(mysql-concpp CONFIG REQUIRED)
  find_package(OpenSSL REQUIRED)
endfunction()

function(llfc_find_client_dependencies)
  find_package(Qt5 CONFIG REQUIRED COMPONENTS Core Gui Network Widgets)
endfunction()

function(llfc_apply_msvc_defaults target_name)
  if(NOT MSVC)
    return()
  endif()
  target_compile_options(${target_name} PRIVATE /utf-8 /wd4819)
  set_target_properties(${target_name} PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
  )
endfunction()

# Resolve repository root from an entry CMakeLists location.
# depth: 0 = repo root, 1 = server/, 2 = server/<svc> or client/llfcchat
function(llfc_get_repo_root out_var depth)
  if(depth EQUAL 0)
    set(_root "${CMAKE_CURRENT_SOURCE_DIR}")
  elseif(depth EQUAL 1)
    get_filename_component(_root "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
  elseif(depth EQUAL 2)
    get_filename_component(_root "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
  else()
    message(FATAL_ERROR "llfc_get_repo_root: unsupported depth ${depth}")
  endif()
  set(${out_var} "${_root}" PARENT_SCOPE)
endfunction()

# Call before project() on standalone entry points.
macro(llfc_setup_standalone_vcpkg depth)
  if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    if(NOT CMAKE_GENERATOR STREQUAL "Ninja Multi-Config")
      message(FATAL_ERROR
        "This project requires the \"Ninja Multi-Config\" generator. "
        "Configure with: cmake -G \"Ninja Multi-Config\" ...")
    endif()
    if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
      message(FATAL_ERROR "In-source builds are not supported. Use a separate build directory.")
    endif()

    llfc_get_repo_root(_llfc_repo_root ${depth})
    set(VCPKG_MANIFEST_DIR "${_llfc_repo_root}" CACHE PATH "vcpkg manifest directory" FORCE)
    set(VCPKG_INSTALLED_DIR "${_llfc_repo_root}/vcpkg_installed" CACHE PATH "vcpkg installed directory" FORCE)
    set(LLFC_REPO_ROOT "${_llfc_repo_root}" CACHE PATH "llfcchat repository root" FORCE)
  endif()
endmacro()

function(llfc_set_runtime_outdir target_name subdir)
  if(NOT DEFINED LLFC_REPO_ROOT)
    message(FATAL_ERROR "LLFC_REPO_ROOT is not set")
  endif()
  set_target_properties(${target_name} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${LLFC_REPO_ROOT}/out/run/$<CONFIG>/${subdir}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${LLFC_REPO_ROOT}/out/run/Debug/${subdir}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${LLFC_REPO_ROOT}/out/run/Release/${subdir}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${LLFC_REPO_ROOT}/out/run/RelWithDebInfo/${subdir}"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${LLFC_REPO_ROOT}/out/run/MinSizeRel/${subdir}"
  )
endfunction()
