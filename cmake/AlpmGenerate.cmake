# Code generation from the installed alpm.h.
#
# The model is parsed from whichever alpm.h the server will actually link
# against, so the wire surface can never drift from the library. Editing the
# overlay, the emitter or upgrading pacman all re-trigger generation.

find_program(ALPMRPC_PYTHON
  NAMES python.exe python3.exe python
  HINTS "${MSYS2_ROOT}/clang64/bin" "${MSYS2_ROOT}/ucrt64/bin"
  DOC "Python with the clang.cindex bindings (MSYS2 ships them in clang64)")

find_file(ALPMRPC_LIBCLANG
  NAMES libclang.dll
  HINTS "${MSYS2_ROOT}/clang64/bin" "${MSYS2_ROOT}/ucrt64/bin"
  DOC "libclang used to parse alpm.h")

find_path(ALPMRPC_CLANG_BUILTIN_INCLUDE stddef.h
  HINTS "${MSYS2_ROOT}/clang64/lib/clang"
  PATH_SUFFIXES 22/include 21/include 20/include 19/include 18/include
  DOC "clang's own builtin include directory")

if(NOT ALPMRPC_PYTHON OR NOT ALPMRPC_LIBCLANG)
  message(FATAL_ERROR
    "Code generation needs python with clang.cindex plus libclang.\n"
    "  pacman -S mingw-w64-clang-x86_64-python-clang "
    "mingw-w64-clang-x86_64-clang")
endif()

# alpm.h is a Cygwin-targeted header; parsing it with a Windows-targeted
# clang trips over __int64 in libarchive and curl. The triple is what makes
# the parse clean, not a workaround for it.
set(ALPMRPC_CLANG_ARGS
  "--clang-arg=--target=x86_64-pc-cygwin"
  "--clang-arg=-I${MSYS2_ROOT}/usr/include")
if(ALPMRPC_CLANG_BUILTIN_INCLUDE)
  list(APPEND ALPMRPC_CLANG_ARGS
    "--clang-arg=-isystem" "--clang-arg=${ALPMRPC_CLANG_BUILTIN_INCLUDE}")
endif()

function(alpmrpc_add_generation OUTDIR)
  set(_gen  "${CMAKE_SOURCE_DIR}/tools/gen")
  set(_hdr  "${MSYS2_ROOT}/usr/include/alpm.h")
  set(_model "${OUTDIR}/api_model.json")

  if(NOT EXISTS "${_hdr}")
    message(FATAL_ERROR "alpm.h not found at ${_hdr}. Is MSYS2_ROOT correct?")
  endif()

  add_custom_command(
    OUTPUT "${_model}"
    COMMAND "${ALPMRPC_PYTHON}" "${_gen}/alpm_api.py"
            --header "${_hdr}" --out "${_model}"
            --libclang "${ALPMRPC_LIBCLANG}" ${ALPMRPC_CLANG_ARGS}
    DEPENDS "${_gen}/alpm_api.py" "${_hdr}"
    COMMENT "Parsing alpm.h into an API model"
    VERBATIM)

  add_custom_command(
    OUTPUT "${OUTDIR}/arpc_dispatch.c"
           "${OUTDIR}/arpc_stubs.c"
           "${OUTDIR}/arpc_handle_tags.h"
           "${OUTDIR}/coverage.json"
    COMMAND "${ALPMRPC_PYTHON}" "${_gen}/emit.py"
            --model "${_model}" --overlay "${_gen}/overlay.json"
            --outdir "${OUTDIR}"
    DEPENDS "${_model}" "${_gen}/emit.py" "${_gen}/overlay.json"
    COMMENT "Emitting server dispatch and client stubs"
    VERBATIM)

  # The unmodified public headers travel with the client. Nothing is patched:
  # ucrt64 supplies its own libarchive headers, so alpm.h compiles as shipped.
  add_custom_command(
    OUTPUT "${OUTDIR}/include/alpm.h" "${OUTDIR}/include/alpm_list.h"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${OUTDIR}/include"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${MSYS2_ROOT}/usr/include/alpm.h"
            "${MSYS2_ROOT}/usr/include/alpm_list.h" "${OUTDIR}/include"
    DEPENDS "${MSYS2_ROOT}/usr/include/alpm.h"
    COMMENT "Staging public libalpm headers"
    VERBATIM)

  add_custom_target(alpmrpc-generate ALL
    DEPENDS "${OUTDIR}/arpc_dispatch.c" "${OUTDIR}/arpc_stubs.c"
            "${OUTDIR}/include/alpm.h")
endfunction()
