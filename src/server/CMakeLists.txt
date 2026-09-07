# alpmrpcd -- the MSYS2-side worker. Configured with cmake/toolchain-msys.cmake.
cmake_minimum_required(VERSION 3.20)
project(alpmrpcd C)

if(NOT ALPMRPC_GEN_DIR)
  message(FATAL_ERROR "ALPMRPC_GEN_DIR must point at the generated sources")
endif()

add_executable(alpmrpcd
  main.c
  arpc_server.c
  arpc_callbacks.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../common/arpc_json.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../common/arpc_b64.c
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

# Windows will not let anything overwrite a running executable, so a rebuild
# fails at this step while a server is up. Working around it by renaming the
# locked file aside would be worse than the error: the old server keeps its
# endpoint, later clients keep connecting to it, and tests quietly run
# against stale code. So retire it instead.
#
# Asking is enough -- the server finishes its current connection and exits,
# and the next client starts a fresh one on demand. If it will not go, fail
# loudly rather than install something that will not be used.
install(CODE "
  set(_stopper \"$<TARGET_FILE:alpmrpcd>\")
  execute_process(
    COMMAND \"\${_stopper}\" --root \"${MSYS2_ROOT}\" --stop
    RESULT_VARIABLE _stop_rc
    OUTPUT_QUIET ERROR_QUIET)
  if(NOT _stop_rc EQUAL 0)
    message(FATAL_ERROR
      \"A running alpmrpcd would not stop, so its binary cannot be \"
      \"replaced. It is probably serving a client that is still \"
      \"connected. Close that client and build again.\")
  endif()
")

install(TARGETS alpmrpcd RUNTIME DESTINATION usr/bin)
