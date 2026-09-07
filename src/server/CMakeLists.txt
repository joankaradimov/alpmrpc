# alpmrpcd -- the MSYS2-side worker. Configured with cmake/toolchain-msys.cmake.
cmake_minimum_required(VERSION 3.20)
project(alpmrpcd C)

if(NOT ALPMRPC_GEN_DIR)
  message(FATAL_ERROR "ALPMRPC_GEN_DIR must point at the generated sources")
endif()

add_executable(alpmrpcd
  main.c
  arpc_server.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../common/arpc_json.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../common/arpc_wire.c
  ${ALPMRPC_GEN_DIR}/arpc_dispatch.c)

target_include_directories(alpmrpcd PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../common
  ${ALPMRPC_GEN_DIR}
  ${MSYS2_ROOT}/usr/include)

target_compile_options(alpmrpcd PRIVATE -Wall -Wextra)

# MSYS2 ships libalpm as a static archive, so its transitive dependencies
# have to be named explicitly.
target_link_libraries(alpmrpcd PRIVATE
  alpm archive curl crypto gpgme intl advapi32)

install(TARGETS alpmrpcd RUNTIME DESTINATION usr/bin)
