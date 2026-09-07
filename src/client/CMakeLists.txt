# The drop-in libalpm shim. Configured with a mingw/clang toolchain (ucrt64,
# mingw64 or clang64) -- nothing here is environment-specific.
cmake_minimum_required(VERSION 3.20)
project(alpmrpc_client C)

if(NOT ALPMRPC_GEN_DIR)
  message(FATAL_ERROR "ALPMRPC_GEN_DIR must point at the generated sources")
endif()

# Matches libalpm's own soname so consumers linking -lalpm resolve normally.
set(ALPM_SOVERSION 14)

add_library(alpm SHARED
  arpc_client.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../common/arpc_json.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../common/arpc_wire.c
  ${ALPMRPC_GEN_DIR}/arpc_stubs.c)

target_include_directories(alpm PUBLIC ${ALPMRPC_GEN_DIR}/include)
target_include_directories(alpm PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../common
  ${ALPMRPC_GEN_DIR})

target_compile_options(alpm PRIVATE -Wall -Wextra)
target_link_libraries(alpm PRIVATE advapi32)

set_target_properties(alpm PROPERTIES
  PREFIX "lib"
  OUTPUT_NAME "alpm-${ALPM_SOVERSION}"
  # Deliberately no msys2 anything: this DLL is loaded into unrelated
  # processes and must import nothing but kernel32/advapi32 and the CRT.
  IMPORT_SUFFIX ".dll.a"
  IMPORT_PREFIX "lib")
set_target_properties(alpm PROPERTIES ARCHIVE_OUTPUT_NAME "alpm")

install(TARGETS alpm RUNTIME DESTINATION bin ARCHIVE DESTINATION lib)
install(FILES ${ALPMRPC_GEN_DIR}/include/alpm.h
              ${ALPMRPC_GEN_DIR}/include/alpm_list.h
        DESTINATION include)

if(ALPMRPC_BUILD_TESTS)
  add_executable(e2e_spike ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/e2e_spike.c)
  target_link_libraries(e2e_spike PRIVATE alpm)
  enable_testing()
  add_test(NAME e2e_spike COMMAND e2e_spike)
endif()
