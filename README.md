# alpmrpc

Exposes an MSYS2-hosted **libalpm** to native mingw/clang/ucrt64 programs.

libalpm only builds under MSYS2, because it needs POSIX facilities Cygwin
emulates and mingw does not (`fork`/`exec` for scriptlets and hooks,
`statvfs`, `pwd.h`, real symlinks and modes). Rather than fork libalpm and
stub those out, the real library keeps running inside MSYS2 and native
callers reach it over a pipe.

    ucrt64 app ──> libalpm-14.dll ──named pipe──> alpmrpcd (MSYS2) ──> libalpm

The client DLL is a drop-in shim exporting the `alpm_*` C ABI. It imports
nothing but kernel32, advapi32 and the UCRT — no MSYS2 DLL is ever loaded
into the calling process.

## Building

    cmake -B build -G Ninja -DMSYS2_ROOT=C:/msys64
    cmake --build build

Configure with a mingw/clang cmake (ucrt64, mingw64, clang64). The superbuild
drives the MSYS2 compiler for the server through a toolchain file, so
`msys/cmake` is not needed.

## Generated, not written

The wire surface is generated from the *installed* `alpm.h`, so it cannot
drift from the library the server links against.

    alpm.h ──libclang──> api_model.json ──┬──> arpc_dispatch.c   (server)
                                          ├──> arpc_stubs.c      (client)
              tools/gen/overlay.json ─────┴──> arpc_handle_tags.h

`tools/gen/overlay.json` is the only hand-maintained input: it records what a
C header cannot express — pointer direction and list ownership. String
ownership is *not* in it, because libalpm's const-ness already determines it
(`char *` is caller-owned, `const char *` is borrowed); `emit.py` asserts that
invariant against each header it parses.

`cmake --build build --target coverage` prints what is generated and, for
everything else, why it is not.

## Design notes

- **No registration.** The endpoint name is derived from the MSYS2 root, the
  protocol version and the user's SID. The client starts the server on first
  use and drops the pipe when the caller's last libalpm handle is released;
  the server exits after an idle period. Nothing persists anywhere.
- **Handles are ids, not pointers.** An `alpm_db_t *` on the client is the
  server's handle id cast to a pointer and is never dereferenced. Ids are
  monotonic and type-tagged, so a stale or wrong-typed id fails a lookup
  instead of reaching libalpm as a bad pointer.
- **The server is single-threaded**, which keeps libalpm's `fork()` for
  scriptlets and hooks on the path Cygwin actually supports.
- `ALPMRPC_TRACE=1` dumps every frame. `alpmrpcd --stdio` runs the dispatch
  layer against stdin/stdout with no IPC at all.

## Status

A spike. 13 of 193 functions are on the wire — enough to exercise every
mechanism end to end (handles, both string ownership models, out-params,
lifetime, type safety). The remaining work is listed in `coverage.json`;
the largest buckets are `alpm_list_t` returns (53) and callbacks (12).
