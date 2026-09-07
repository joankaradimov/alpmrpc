# Builds the server with the MSYS2 (Cygwin) toolchain. This is the only place
# that toolchain is named; everything else is ordinary CMake.
set(CMAKE_SYSTEM_NAME Windows)

if(NOT MSYS2_ROOT)
  set(MSYS2_ROOT "$ENV{MSYS2_ROOT}")
endif()
if(NOT MSYS2_ROOT)
  message(FATAL_ERROR "toolchain-msys.cmake needs MSYS2_ROOT")
endif()

# try_compile() re-reads this file in a scratch project that does not
# inherit our cache, so MSYS2_ROOT has to be forwarded explicitly.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES MSYS2_ROOT)

set(CMAKE_C_COMPILER   "${MSYS2_ROOT}/usr/bin/gcc.exe")
set(CMAKE_CXX_COMPILER "${MSYS2_ROOT}/usr/bin/g++.exe")

# Look for libalpm and its dependencies in the MSYS2 tree only. Picking up a
# mingw libarchive here would produce a server that cannot load.
set(CMAKE_FIND_ROOT_PATH "${MSYS2_ROOT}/usr")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
