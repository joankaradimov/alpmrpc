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
  arpc_local.c
  vendor/alpm_list.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../common/arpc_json.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../common/arpc_b64.c
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
                           ${CMAKE_CURRENT_SOURCE_DIR}/../common/arpc_json.c
                           ${CMAKE_CURRENT_SOURCE_DIR}/../common/arpc_b64.c)
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

  # A transaction has to be allowed to change something, so it runs against
  # a throwaway root rather than the real installation. The fixture is built
  # by MSYS2's bash: the packages are made with bsdtar, and the shell that
  # goes inside the root -- scriptlets and hooks are chrooted there -- is
  # MSYS2's own. Rebuilt before every run, because the test installs into it.
  if(MSYS2_ROOT AND EXISTS "${MSYS2_ROOT}/usr/bin/bash.exe")
    set(_scratch ${CMAKE_CURRENT_BINARY_DIR}/scratch)

    add_test(NAME scratch_root
             COMMAND "${MSYS2_ROOT}/usr/bin/bash.exe"
                     "${_tests}/scratch/mkroot.sh" "${_scratch}")
    set_tests_properties(scratch_root PROPERTIES FIXTURES_SETUP scratch)

    add_executable(e2e_transaction ${_tests}/e2e_transaction.c)
    target_link_libraries(e2e_transaction PRIVATE alpm)
    add_test(NAME e2e_transaction COMMAND e2e_transaction "${_scratch}")
    set_tests_properties(e2e_transaction PROPERTIES FIXTURES_REQUIRED scratch)

    # Rebuilds the fixture itself between runs, so it is not part of ctest.
    add_executable(bench_transaction ${_tests}/bench_transaction.c)
    target_link_libraries(bench_transaction PRIVATE alpm)
    set_target_properties(bench_transaction PROPERTIES
      # bench_transaction <root> <mkroot.sh> <msys2 root>
      FOLDER bench)
  else()
    message(STATUS
      "No MSYS2 bash: skipping the transaction test, which needs a fixture root")
  endif()

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
