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
  arpc_callbacks.c
  vendor/alpm_list.c
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
  enable_testing()
  set(_tests ${CMAKE_CURRENT_SOURCE_DIR}/../../tests)

  # Codec round-trip tests: no server, no pipe, just the encoder and reader.
  add_executable(test_json ${_tests}/test_json.c
                           ${CMAKE_CURRENT_SOURCE_DIR}/../common/arpc_json.c)
  target_include_directories(test_json PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../common)
  add_test(NAME test_json COMMAND test_json)

  # Needs a reachable server; it launches one on demand.
  add_executable(e2e_spike ${_tests}/e2e_spike.c)
  target_link_libraries(e2e_spike PRIVATE alpm)
  add_test(NAME e2e_spike COMMAND e2e_spike)

  add_executable(e2e_lists ${_tests}/e2e_lists.c)
  target_link_libraries(e2e_lists PRIVATE alpm)
  add_test(NAME e2e_lists COMMAND e2e_lists)

  add_executable(e2e_callbacks ${_tests}/e2e_callbacks.c)
  target_link_libraries(e2e_callbacks PRIVATE alpm)
  add_test(NAME e2e_callbacks COMMAND e2e_callbacks)

  add_executable(bench_pkg_fields ${_tests}/bench_pkg_fields.c)
  target_link_libraries(bench_pkg_fields PRIVATE alpm)

  add_executable(bench_json_access ${_tests}/bench_json_access.c
                                  ${CMAKE_CURRENT_SOURCE_DIR}/../common/arpc_json.c)
  target_include_directories(bench_json_access PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../common)

  add_executable(bench_codec ${_tests}/bench_codec.c
                             ${CMAKE_CURRENT_SOURCE_DIR}/../common/arpc_json.c)
  target_include_directories(bench_codec PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../common)
  target_link_libraries(bench_codec PRIVATE alpm)
endif()
