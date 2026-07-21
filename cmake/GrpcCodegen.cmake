# Protobuf/gRPC code generation into the build tree.
include_guard(GLOBAL)

function(llfc_add_proto_library target_name proto_file)
  if(TARGET ${target_name})
    return()
  endif()

  if(NOT EXISTS "${proto_file}")
    message(FATAL_ERROR "llfc_add_proto_library: proto file not found: ${proto_file}")
  endif()

  get_filename_component(_proto_abs "${proto_file}" ABSOLUTE)
  get_filename_component(_proto_dir "${_proto_abs}" DIRECTORY)
  get_filename_component(_proto_name "${_proto_abs}" NAME_WE)

  set(_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/generated/${target_name}")
  set(_pb_cc "${_gen_dir}/${_proto_name}.pb.cc")
  set(_pb_h  "${_gen_dir}/${_proto_name}.pb.h")
  set(_grpc_cc "${_gen_dir}/${_proto_name}.grpc.pb.cc")
  set(_grpc_h  "${_gen_dir}/${_proto_name}.grpc.pb.h")

  add_custom_command(
    OUTPUT "${_pb_cc}" "${_pb_h}" "${_grpc_cc}" "${_grpc_h}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_gen_dir}"
    COMMAND "$<TARGET_FILE:protobuf::protoc>"
            "--proto_path=${_proto_dir}"
            "--cpp_out=${_gen_dir}"
            "--grpc_out=${_gen_dir}"
            "--plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>"
            "${_proto_abs}"
    DEPENDS "${_proto_abs}" protobuf::protoc gRPC::grpc_cpp_plugin
    COMMENT "Generating gRPC/protobuf sources for ${target_name}"
    VERBATIM
  )

  add_library(${target_name} STATIC
    "${_pb_cc}" "${_pb_h}" "${_grpc_cc}" "${_grpc_h}"
  )
  target_include_directories(${target_name} PUBLIC "${_gen_dir}")
  target_link_libraries(${target_name} PUBLIC
    protobuf::libprotobuf
    gRPC::grpc++
  )
  target_compile_features(${target_name} PUBLIC cxx_std_17)
  llfc_apply_msvc_defaults(${target_name})
endfunction()

function(llfc_ensure_control_proto)
  if(NOT DEFINED LLFC_REPO_ROOT)
    message(FATAL_ERROR "LLFC_REPO_ROOT is not set")
  endif()
  llfc_add_proto_library(llfc_control_proto
    "${LLFC_REPO_ROOT}/server/proto/control/message.proto")
endfunction()

function(llfc_ensure_chat_proto)
  if(NOT DEFINED LLFC_REPO_ROOT)
    message(FATAL_ERROR "LLFC_REPO_ROOT is not set")
  endif()
  llfc_add_proto_library(llfc_chat_proto
    "${LLFC_REPO_ROOT}/server/proto/chat/message.proto")
endfunction()
