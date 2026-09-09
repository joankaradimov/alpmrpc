# alpmrpc -- superbuild.
#
# Two toolchains have to cooperate here: the server must be built with the
# MSYS2 (Cygwin) compiler, because that is the only environment libalpm
# builds in; the client must be built with a mingw/clang compiler, because
# that is where it will be loaded. Neither can build the other, so this
# superbuild configures each as its own project and only shares the
# generated sources between them.
#
#   cmake -B build -G Ninja -DMSYS2_ROOT=C:/dev/msys64
#   cmake --build build
#
# Configure this with a mingw/clang cmake (ucrt64, mingw64, clang64). It
# drives the MSYS2 compiler through a toolchain file, so msys/cmake is not
# required.
cmake_minimum_required(VERSION 3.20)
project(alpmrpc NONE)

include(ExternalProject)

# --- where MSYS2 lives -------------------------------------------------------

set(MSYS2_ROOT "" CACHE PATH "Root of the MSYS2 installation to manage")
if(NOT MSYS2_ROOT)
  foreach(_candidate C:/msys64 C:/dev/msys64 C:/tools/msys64 D:/msys64)
    if(EXISTS "${_candidate}/usr/include/alpm.h")
      set(MSYS2_ROOT "${_candidate}")
      break()
    endif()
  endforeach()
endif()
if(NOT MSYS2_ROOT OR NOT EXISTS "${MSYS2_ROOT}/usr/include/alpm.h")
  message(FATAL_ERROR
    "Set -DMSYS2_ROOT=<path> to an MSYS2 install that has libalpm headers "
    "(pacman -S libalpm-devel).")
endif()
message(STATUS "MSYS2 root: ${MSYS2_ROOT}")

option(ALPMRPC_BUILD_TESTS "Build the end-to-end test" ON)
# The form the client's paths take is decided here, once. libalpm's are the
# server's, POSIX; every client known so far wants Win32, and the server
# converts for a client that asks, so the default client asks.
option(ALPMRPC_WIN32_PATHS
  "Build a client that takes and returns Win32 paths, converted on the server"
  ON)

# --- generation (shared by both sides) --------------------------------------

set(ALPMRPC_GEN_DIR "${CMAKE_BINARY_DIR}/gen")
file(MAKE_DIRECTORY "${ALPMRPC_GEN_DIR}")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(AlpmGenerate)
alpmrpc_add_generation("${ALPMRPC_GEN_DIR}")

# --- server: MSYS2 toolchain ------------------------------------------------

ExternalProject_Add(server
  SOURCE_DIR   "${CMAKE_CURRENT_SOURCE_DIR}/src/server"
  BINARY_DIR   "${CMAKE_BINARY_DIR}/server"
  INSTALL_DIR  "${CMAKE_BINARY_DIR}/stage"
  DEPENDS      alpmrpc-generate
  CMAKE_ARGS
    -DCMAKE_TOOLCHAIN_FILE=${CMAKE_CURRENT_SOURCE_DIR}/cmake/toolchain-msys.cmake
    -DMSYS2_ROOT=${MSYS2_ROOT}
    -DALPMRPC_GEN_DIR=${ALPMRPC_GEN_DIR}
    -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
    -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
  BUILD_ALWAYS 1)

# --- client: whichever mingw/clang toolchain configured this superbuild -----

ExternalProject_Add(client
  SOURCE_DIR   "${CMAKE_CURRENT_SOURCE_DIR}/src/client"
  BINARY_DIR   "${CMAKE_BINARY_DIR}/client"
  INSTALL_DIR  "${CMAKE_BINARY_DIR}/stage"
  DEPENDS      alpmrpc-generate
  CMAKE_ARGS
    -DALPMRPC_GEN_DIR=${ALPMRPC_GEN_DIR}
    -DALPMRPC_BUILD_TESTS=${ALPMRPC_BUILD_TESTS}
    -DALPMRPC_WIN32_PATHS=${ALPMRPC_WIN32_PATHS}
    # Only the transaction test needs this, to build its fixture root with
    # MSYS2's own bash. The client itself links nothing from there.
    -DMSYS2_ROOT=${MSYS2_ROOT}
    -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
    -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
  BUILD_ALWAYS 1)

# --- coverage report --------------------------------------------------------

add_custom_target(coverage
  COMMAND "${CMAKE_COMMAND}" -E cat "${ALPMRPC_GEN_DIR}/coverage.json"
  DEPENDS alpmrpc-generate
  COMMENT "Generated vs skipped libalpm surface"
  VERBATIM)
