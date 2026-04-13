cmake_minimum_required(VERSION 3.16)
project(llm_inference_gateway LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ============================================================================
# Dependencies: gRPC and Protobuf
#
# Default: builds gRPC from source via FetchContent (no system deps needed).
# Optional: pass -DUSE_SYSTEM_GRPC=ON to use a pre-installed gRPC for faster builds.
# ============================================================================

option(USE_SYSTEM_GRPC "Use system-installed gRPC instead of building from source" OFF)

if(USE_SYSTEM_GRPC)
  # ---- System-installed gRPC (opt-in for faster builds) ----
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GRPC REQUIRED grpc++)
  pkg_check_modules(PROTOBUF REQUIRED protobuf)

  # Find protoc and grpc_cpp_plugin from the same prefix as the libraries
  execute_process(
    COMMAND ${PKG_CONFIG_EXECUTABLE} --variable=exec_prefix protobuf
    OUTPUT_VARIABLE _PROTOBUF_PREFIX
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  execute_process(
    COMMAND ${PKG_CONFIG_EXECUTABLE} --variable=exec_prefix grpc++
    OUTPUT_VARIABLE _GRPC_PREFIX
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  find_program(_PROTOBUF_PROTOC protoc HINTS "${_PROTOBUF_PREFIX}/bin" NO_DEFAULT_PATH REQUIRED)
  find_program(_GRPC_CPP_PLUGIN grpc_cpp_plugin HINTS "${_GRPC_PREFIX}/bin" NO_DEFAULT_PATH REQUIRED)

  message(STATUS "Using system gRPC (${GRPC_VERSION}), protoc: ${_PROTOBUF_PROTOC}")

  add_library(grpc_deps INTERFACE)
  target_include_directories(grpc_deps INTERFACE ${GRPC_INCLUDE_DIRS} ${PROTOBUF_INCLUDE_DIRS})
  target_link_directories(grpc_deps INTERFACE ${GRPC_LIBRARY_DIRS} ${PROTOBUF_LIBRARY_DIRS})
  target_link_libraries(grpc_deps INTERFACE ${GRPC_LIBRARIES} ${PROTOBUF_LIBRARIES} grpc++_reflection)

  set(_PROTO_DEPENDS "")
else()
  # ---- Build gRPC from source via FetchContent  ----
  message(STATUS "Building gRPC from source via FetchContent (this will take a while on first build)")
  include(FetchContent)

  set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)
  set(ABSL_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(protobuf_INSTALL OFF CACHE BOOL "" FORCE)
  set(protobuf_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(utf8_range_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(gRPC_INSTALL OFF CACHE BOOL "" FORCE)
  set(gRPC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(gRPC_BUILD_GRPC_NODE_PLUGIN OFF CACHE BOOL "" FORCE)
  set(gRPC_BUILD_GRPC_OBJECTIVE_C_PLUGIN OFF CACHE BOOL "" FORCE)
  set(gRPC_BUILD_GRPC_PHP_PLUGIN OFF CACHE BOOL "" FORCE)
  set(gRPC_BUILD_GRPC_PYTHON_PLUGIN OFF CACHE BOOL "" FORCE)
  set(gRPC_BUILD_GRPC_RUBY_PLUGIN OFF CACHE BOOL "" FORCE)
  set(gRPC_BUILD_CSHARP_EXT OFF CACHE BOOL "" FORCE)

  FetchContent_Declare(
    grpc
    GIT_REPOSITORY https://github.com/grpc/grpc
    GIT_TAG        v1.62.1
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(grpc)

  set(_PROTOBUF_PROTOC $<TARGET_FILE:protoc>)
  set(_GRPC_CPP_PLUGIN $<TARGET_FILE:grpc_cpp_plugin>)

  add_library(grpc_deps INTERFACE)
  target_link_libraries(grpc_deps INTERFACE grpc++ grpc++_reflection protobuf::libprotobuf)

  set(_PROTO_DEPENDS protoc grpc_cpp_plugin)
endif()

# ============================================================================
# Proto file compilation
# ============================================================================
set(PROTO_DIR "${CMAKE_SOURCE_DIR}/proto")
set(PROTO_GEN_DIR "${CMAKE_BINARY_DIR}/proto_gen")
file(MAKE_DIRECTORY ${PROTO_GEN_DIR})

function(compile_proto PROTO_NAME)
  set(PROTO_FILE "${PROTO_DIR}/${PROTO_NAME}.proto")
  set(PB_CC "${PROTO_GEN_DIR}/${PROTO_NAME}.pb.cc")
  set(PB_H "${PROTO_GEN_DIR}/${PROTO_NAME}.pb.h")
  set(GRPC_CC "${PROTO_GEN_DIR}/${PROTO_NAME}.grpc.pb.cc")
  set(GRPC_H "${PROTO_GEN_DIR}/${PROTO_NAME}.grpc.pb.h")

  add_custom_command(
    OUTPUT ${PB_CC} ${PB_H} ${GRPC_CC} ${GRPC_H}
    COMMAND ${_PROTOBUF_PROTOC}
      --proto_path=${PROTO_DIR}
      --cpp_out=${PROTO_GEN_DIR}
      --grpc_out=${PROTO_GEN_DIR}
      --plugin=protoc-gen-grpc=${_GRPC_CPP_PLUGIN}
      ${PROTO_FILE}
    DEPENDS ${PROTO_FILE} ${_PROTO_DEPENDS}
    COMMENT "Compiling ${PROTO_NAME}.proto"
  )

  set(${PROTO_NAME}_SRCS ${PB_CC} ${GRPC_CC} PARENT_SCOPE)
  set(${PROTO_NAME}_HDRS ${PB_H} ${GRPC_H} PARENT_SCOPE)
endfunction()

compile_proto(llmgateway)
compile_proto(gossip)

# Proto library (shared by all targets)
add_library(proto_lib STATIC
  ${llmgateway_SRCS}
  ${gossip_SRCS}
)
target_include_directories(proto_lib PUBLIC ${PROTO_GEN_DIR})
target_link_libraries(proto_lib PUBLIC grpc_deps)

# Gossip library (SWIM protocol, used by replica and gateway)
add_library(gossip_lib STATIC
  src/gossip/udp_transport.cpp
  src/gossip/membership_list.cpp
  src/gossip/swim.cpp
)
target_include_directories(gossip_lib PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(gossip_lib PUBLIC proto_lib)

# ============================================================================
# Source targets
# ============================================================================

add_executable(replica
  src/replica/main.cpp
  src/replica/replica_server.cpp
)
target_include_directories(replica PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(replica PRIVATE proto_lib gossip_lib)

add_executable(gateway
  src/gateway/main.cpp
  src/gateway/replica_registry.cpp
  src/gateway/load_balancer.cpp
  src/gateway/circuit_breaker.cpp
)
target_include_directories(gateway PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(gateway PRIVATE proto_lib gossip_lib)

add_executable(client
  src/client/main.cpp
  src/client/client.cpp
)
target_include_directories(client PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(client PRIVATE proto_lib)

# Unit tests for gossip components
add_executable(test_gossip_unit tests/test_gossip_unit.cpp)
target_include_directories(test_gossip_unit PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_gossip_unit PRIVATE gossip_lib)

# SWIM integration tests
add_executable(test_swim_integration tests/test_swim_integration.cpp)
target_include_directories(test_swim_integration PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_swim_integration PRIVATE gossip_lib)
