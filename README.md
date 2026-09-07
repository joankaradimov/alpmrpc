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
- **Lists are real.** `alpm_list_t` is transparent -- callers walk `->next`
  and call `alpm_list_count`/`alpm_list_free` directly -- so it cannot be
  proxied. The server serialises a whole list into one array and the client
  materialises a genuine chain, linking the real `alpm_list.c` (vendored, see
  `src/client/vendor/`). A list therefore costs one round trip, not one per
  element. Transparent element structs are materialised field by field,
  recursively: `alpm_group_t` arrives with its `packages` list attached.
- **Package fields are fetched a column at a time.** Reading eight fields of
  1150 packages one accessor call at a time is 9200 round trips. A
  materialised package list registers itself as a group, and the first read
  of any field fetches that field for the whole group in one call, so the
  1149 reads that follow are memory reads (~50ns). Fields nobody touches are
  never fetched. `alpm_pkg_get_name()` is unchanged from the caller's side --
  this is not a new API, it is the same one not going to the wire.
  Which accessors are cheap enough to batch is an overlay rule: the excluded
  ones hash a package file or compute a download size.
- **Structs cross in both directions.** The server writes a record and the
  client materialises it; the client writes one and the server rebuilds a
  temporary for the duration of the call. As with lists, `const` marks the
  read-only inputs, and a non-const struct param is skipped rather than
  guessed at. Two records are refused outright -- `alpm_filelist_t` and
  `alpm_siglist_t` are a count plus an array, not a pointer to one struct,
  and the generator would otherwise materialise exactly one element.
- **`alpm_dep_free` and friends never reach the server.** They free a struct
  this client materialised; the server has libalpm's own copy, which is not
  ours to free. The generated stub calls the matching free helper locally.
- **Callbacks run nested inside the call that triggered them.** libalpm
  calls back from the middle of an operation, and a question has to be
  answered before the transaction that asked it can continue. So the server
  installs its own trampoline with libalpm, sends the arguments up the same
  pipe, and blocks; the client's frame loop delivers it to the caller's
  function pointer and replies before its own call returns. That ordering is
  not an optimisation -- it is the only shape a synchronous callback can
  take -- and it keeps the server single-threaded, which is what keeps
  libalpm's `fork()` on the path Cygwin supports.
- **Borrowed lists are cached against their owner** and looked up *before*
  the call, because libalpm hands back the same pointer for repeated calls
  and a caller may still be holding an earlier one. Caller-owned lists are
  built fresh and freed by the caller with the usual idiom.
- **The server is single-threaded**, which keeps libalpm's `fork()` for
  scriptlets and hooks on the path Cygwin actually supports.
- `ALPMRPC_TRACE=1` dumps every frame. `alpmrpcd --stdio` runs the dispatch
  layer against stdin/stdout with no IPC at all.
- **Rebuilding while a server is up.** Windows will not overwrite a running
  executable, so installing asks any server on this endpoint to exit first
  (`alpmrpcd --stop`); the next client starts a fresh one on demand. Renaming
  the locked file aside would have been the easier fix and a worse one -- the
  old server would keep its endpoint and later clients would keep reaching
  it, so tests would quietly run against stale code. If a client is still
  connected the server cannot be retired, and the build says so rather than
  installing something that will not be used.

## Tests

`ctest` from the client build directory runs codec round-trip tests and the
end-to-end test (which launches a server on demand). `bench_codec` reports
round-trip cost, the codec's share of it, and throughput on a bulk payload --
run it before and after anything that touches the wire.

## Status

158 of 193 functions are generated, plus the twelve hand-written callback
setters and getters. `coverage.json` lists everything else with a reason for
each; what is left is small and specific — the two counted-array records,
the opaque `void *` cursors (changelog, mtree), and `alpm_logaction`'s `...`.

Carried callbacks: `logcb`, `progresscb`, `eventcb`, `questioncb`. `dlcb` and
`fetchcb` are not, and their setters return -1 rather than accepting a
callback that would then silently never fire.

The read path works and is measured. Two things are written but not proven,
and are listed here as untested code rather than working code:

- **No question has ever fired.** All eight variants are marshalled and
  registration is tested — `alpm_option_set_questioncb` returning 0 is what
  says a trampoline was installed — but a question needs libalpm to have
  something to ask, and none of the cheap triggers arise on a healthy
  up-to-date install. Demonstrating it needs a scratch pacman root built to
  contain a conflict.
- **libalpm `fork()`s for scriptlets and hooks, and that has never been
  exercised through this bridge.** The single-threaded server exists to keep
  that fork on the path Cygwin supports; that the arrangement works is a
  design argument, not a measurement.
